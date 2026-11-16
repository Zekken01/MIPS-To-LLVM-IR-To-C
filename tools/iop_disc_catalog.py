#!/usr/bin/env python3
"""Resolve guest cdrom0: IRX references against an extracted PS2 disc folder.

Static ELF references are *candidates*, not confirmed SifLoadModule requests
or a safe IOP startup order. Nothing here starts or links an IRX. Resolution
exists so a future guest-triggered LOADFILE RPC can access actual disc files
without hardcoded host paths, guesswork or path traversal.
"""

import argparse
import json
import re
from pathlib import Path

from analyze_iop_module_paths import analyze_elf

CDROM_PATH = re.compile(r"(?i)^cdrom0:[\\/]*(.+)$")
IRX_NAME = re.compile(r"(?i)^(.+\.irx)(?:;[0-9]+)?$")
MAX_CANDIDATES = 256
MAX_IRX_BYTES = 2 * 1024 * 1024


def parse_cdrom_path(guest_path):
    """Return validated, host-independent path components or None."""
    if not isinstance(guest_path, str):
        return None
    match = CDROM_PATH.fullmatch(guest_path)
    if match is None:
        return None

    text = match.group(1).replace("\\", "/")
    names = text.split("/")
    if (not names or len(names) > 16 or
            any(part in ("", ".", "..") or ":" in part or
                len(part) > 128 or any(ord(ch) < 32 or ord(ch) > 126
                                        for ch in part) for part in names)):
        return None

    final = IRX_NAME.fullmatch(names[-1])
    if final is None:
        return None
    return tuple(names)


def resolve_disc_path(root, guest_path):
    """Return path only if every case-insensitive component is unambiguous."""
    parts = parse_cdrom_path(guest_path)
    if parts is None:
        return None

    root = Path(root).resolve(strict=True)
    if not root.is_dir():
        raise ValueError(f"disc root is not a directory: {root}")

    current = root
    for name in parts:
        if not current.is_dir():
            return None
        # Extractors may preserve ISO9660 ";1" suffixes or strip them.
        # Never silently choose if both representations exist.
        accepted = {name.casefold()}
        if name == parts[-1]:
            suffix = IRX_NAME.fullmatch(name)
            if suffix is not None:
                accepted.add(suffix.group(1).casefold())
        matches = [
            item for item in current.iterdir()
            if item.name.casefold() in accepted
        ]
        if len(matches) > 1:
            raise ValueError(
                f"ambiguous case-insensitive disc path: {guest_path}"
            )
        if not matches:
            return None
        target = matches[0]
        # Reject symlinks: a disc folder may not redirect guest reads to
        # arbitrary files outside the declared game root.
        if target.is_symlink():
            raise ValueError(f"symlink inside disc tree: {target}")
        current = target

    if not current.is_file():
        return None
    size = current.stat().st_size
    if size < 52 or size > MAX_IRX_BYTES:
        raise ValueError(f"IRX file size is invalid: {current} ({size})")
    with current.open("rb") as stream:
        header = stream.read(52)
    if (header[:6] != b"\x7fELF\x01\x01" or
            int.from_bytes(header[18:20], "little") != 8):
        raise ValueError(f"not a little-endian MIPS IRX: {current}")
    return current


def build_catalog(elf, disc_root=None):
    elf = Path(elf).resolve(strict=True)
    root = Path(disc_root).resolve(strict=True) if disc_root else elf.parent
    evidence = analyze_elf(elf.read_bytes())
    if len(evidence["module_paths"]) > MAX_CANDIDATES:
        raise ValueError("unreasonably large ELF module candidate list")

    candidates = []
    for item in evidence["module_paths"]:
        guest_path = item["path"]
        kind = "cdrom" if parse_cdrom_path(guest_path) else "unsupported"
        resolved = None
        if kind == "cdrom":
            resolved = resolve_disc_path(root, guest_path)
        candidates.append({
            "guest_path": guest_path,
            "guest_address": item["guest_address"],
            "source": "elf-static-candidate",
            "status": "resolved" if resolved else (
                "missing" if kind == "cdrom" else "unsupported-device"
            ),
            "host_path": str(resolved) if resolved else None,
            "static_call_candidates": len(item.get("call_candidates", [])),
        })

    return {
        "schema": "iop-disc-catalog-v1",
        "elf_sha256": evidence["elf_sha256"],
        "disc_root": str(root),
        "candidates": candidates,
        "resolved_count": sum(
            item["status"] == "resolved" for item in candidates
        ),
        "runtime_load_count": 0,
        "limitations": [
            "Only ELF-embedded cdrom0: IRX paths are discovered.",
            "Static candidates do not establish runtime load order.",
            "No IOP firmware, module imports or IRX entrypoints are executed.",
            "Runtime LOADFILE RPC handling is not yet attached to the catalog.",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--disc-root", type=Path)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = build_catalog(args.elf, args.disc_root)
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
    except (OSError, ValueError) as exc:
        parser.exit(1, f"IOP disc catalog failed: {exc}\n")
    print(
        f"IOP disc candidates: {len(report['candidates'])}, "
        f"resolved: {report['resolved_count']} "
        "(no modules loaded or executed)"
    )
    print(f"IOP disc catalog: {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
