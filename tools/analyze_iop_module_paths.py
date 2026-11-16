#!/usr/bin/env python3
"""Discover candidate IOP IRX paths embedded in a PS2 MIPS ELF.

This is static evidence, NOT a record of actual SifLoadModule calls. Some
paths are constructed at runtime, supplied by IOP code, or loaded through
SIF RPC without an ELF string; those require guest execution tracing.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


PATH_SUFFIX = re.compile(rb"(?i)\.irx(?:;[0-9]+)?$")
PRINTABLE = re.compile(rb"[\x20-\x7e]{4,255}\x00")
LOADFILE_SERVER_ID = 0x80000006
BRANCH_OPS = {1, 2, 3, 4, 5, 6, 7}
PAIR_OPS = {9: "addiu", 13: "ori"}
MASK32 = 0xffffffff
PHYSICAL_MASK = 0x1fffffff


@dataclass(frozen=True)
class Segment:
    file_offset: int
    address: int
    file_size: int
    flags: int


def parse_elf(raw):
    if (len(raw) < 52 or raw[:4] != b"\x7fELF" or
            raw[4] != 1 or raw[5] != 1 or
            struct.unpack_from("<H", raw, 18)[0] != 8):
        raise ValueError("expected a little-endian ELF32 MIPS executable")

    ph_offset = struct.unpack_from("<I", raw, 28)[0]
    ph_entry_size, ph_count = struct.unpack_from("<HH", raw, 42)
    if ph_entry_size < 32 or ph_count == 0 or ph_count > 128 or (
            ph_offset + ph_entry_size * ph_count > len(raw)):
        raise ValueError("invalid ELF program header table")

    segments = []
    for index in range(ph_count):
        offset = ph_offset + index * ph_entry_size
        kind, file_offset, address, _, file_size, mem_size, flags, _ = (
            struct.unpack_from("<IIIIIIII", raw, offset))
        if kind != 1:
            continue
        if (file_size > mem_size or file_offset + file_size > len(raw) or
                address + file_size > 0x100000000):
            raise ValueError(f"PT_LOAD #{index} outside ELF/guest memory")
        if file_size:
            segments.append(Segment(file_offset, address, file_size, flags))
    if not segments:
        raise ValueError("ELF contains no file-backed PT_LOAD segments")
    return segments


def find_module_paths(raw, segments):
    paths = {}
    for segment in segments:
        section = raw[segment.file_offset:
                      segment.file_offset + segment.file_size]
        for match in PRINTABLE.finditer(section):
            text = match.group()[:-1]
            if not PATH_SUFFIX.search(text):
                continue
            # Keep exact case and device prefix; never reinterpret a
            # candidate as a confirmed request or a filesystem path.
            address = segment.address + match.start()
            key = (address, text)
            paths[key] = {
                "path": text.decode("ascii"),
                "guest_address": address,
                "file_offset": segment.file_offset + match.start(),
                "references": [],
            }
    return sorted(paths.values(), key=lambda item: item["guest_address"])


def branch_instruction(word):
    op = word >> 26
    if op in BRANCH_OPS:
        return True
    if op == 0 and (word & 63) in (8, 9):  # JR, JALR
        return True
    return False


def writes_register(word, register):
    """Conservative clobber check for LUI's source register."""
    op = word >> 26
    if op == 0:
        function = word & 63
        if function in (8, 12, 13, 17, 19, 24, 25, 26, 27):
            return False
        return (word >> 11) & 31 == register
    if op == 3:  # JAL writes $ra
        return register == 31
    if 8 <= op <= 15 or 32 <= op <= 39 or op in (1, 16, 17, 18):
        return (word >> 16) & 31 == register
    return False


def addresses_match(value, address):
    if value == address:
        return True
    # KSEG0/KSEG1 aliases can point to the same physical RAM as the
    # executable's low virtual address. Never alias arbitrary addresses.
    if (value & 0xe0000000) in (0x80000000, 0xa0000000):
        return (value & PHYSICAL_MASK) == address
    if (address & 0xe0000000) in (0x80000000, 0xa0000000):
        return (address & PHYSICAL_MASK) == value
    return False


def find_address_constructions(raw, segments, targets):
    sites = {value: [] for value in targets}
    for segment in segments:
        if not (segment.flags & 1):  # ELF PF_X
            continue
        start = (4 - segment.address % 4) % 4
        section = raw[segment.file_offset:
                      segment.file_offset + segment.file_size]
        words = []
        for at in range(start, len(section) - 3, 4):
            words.append((segment.address + at,
                          struct.unpack_from("<I", section, at)[0]))
        for index, (lui_pc, lui) in enumerate(words):
            if lui >> 26 != 15 or (lui >> 21) & 31 or not ((lui >> 16) & 31):
                continue
            register = (lui >> 16) & 31
            high = (lui & 0xffff) << 16
            for pc, opcode in words[index + 1:index + 13]:
                if branch_instruction(opcode):
                    break
                kind = opcode >> 26
                rt = (opcode >> 16) & 31
                rs = (opcode >> 21) & 31
                if kind in PAIR_OPS and rs == register and rt:
                    immediate = opcode & 0xffff
                    if kind == 9 and immediate >= 0x8000:
                        immediate -= 0x10000
                    value = ((high + immediate) if kind == 9
                             else (high | immediate)) & MASK32
                    for address in targets:
                        if addresses_match(value, address):
                            sites[address].append({
                                "lui_pc": lui_pc,
                                "use_pc": pc,
                                "register": rt,
                                "construction": "lui+" + PAIR_OPS[kind],
                                "computed_address": value,
                            })
                if writes_register(opcode, register):
                    break
    for value in sites:
        sites[value] = sorted(
            sites[value], key=lambda item: (item["lui_pc"], item["use_pc"]))
    return sites


def analyze_elf(raw):
    segments = parse_elf(raw)
    paths = find_module_paths(raw, segments)
    addresses = {entry["guest_address"] for entry in paths}
    addresses.add(LOADFILE_SERVER_ID)
    references = find_address_constructions(raw, segments, addresses)
    for entry in paths:
        entry["references"] = references[entry["guest_address"]]
    return {
        "schema": 1,
        "elf_sha256": hashlib.sha256(raw).hexdigest(),
        "module_paths": paths,
        "loadfile_rpc_server_id": LOADFILE_SERVER_ID,
        "loadfile_rpc_server_references": references[LOADFILE_SERVER_ID],
        "limitations": [
            "Embedded IRX strings are candidates, not confirmed runtime loads.",
            "A MIPS LUI+ADDIU/ORI pointer is a static reference, not a call.",
            "Dynamically constructed paths and IOP-originated loads are not discoverable from ELF strings alone.",
            "Actual SifLoadModule requests use LOADFILE SIF RPC (server 0x80000006); capture RPC packets to confirm requests and ordering.",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="Local game ELF (for example PS2_GAME/SLUS_214.42)")
    parser.add_argument("--json", type=Path, help="Write machine-readable evidence report")
    args = parser.parse_args(argv)

    try:
        report = analyze_elf(args.elf.read_bytes())
        if args.json is not None:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"IOP path discovery failed: {exc}\n")

    print(f"ELF SHA-256: {report['elf_sha256']}")
    print(f"Embedded IRX candidate strings: {len(report['module_paths'])}")
    for item in report["module_paths"]:
        print(f"  0x{item['guest_address']:08X} {item['path']!r} "
              f"(static references: {len(item['references'])})")
        for site in item["references"][:8]:
            print(f"    0x{site['lui_pc']:08X} -> 0x{site['use_pc']:08X} "
                  f"{site['construction']} = 0x{site['computed_address']:08X}")
        if len(item["references"]) > 8:
            print(f"    ... {len(item['references']) - 8} more references in JSON")
    print("LOADFILE RPC server constant 0x80000006: "
          f"{len(report['loadfile_rpc_server_references'])} static references")
    print("Note: these are static candidates, NOT a confirmed load order.")
    if args.json is not None:
        print(f"Report: {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
