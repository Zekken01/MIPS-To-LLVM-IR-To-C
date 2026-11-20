#!/usr/bin/env python3
"""Bounded static IOP IRX import inventory; never resolves or executes modules.

Mirrors the strict import-table signature in src/iop/irx_loader.cpp:
0x41e00000 header, eight-byte library at +12, 20-byte header,
then consecutive JR RA / ADDIU zero,zero,ordinal import stubs.
Only file-backed PT_LOAD ranges are scanned. Findings are static
dependencies, not proof of execution or load order.
"""

import struct

MAX_IRX_BYTES = 2 * 1024 * 1024
MAX_PROGRAM_HEADERS = 64
MAX_IMPORTS = 4096
IMPORT_MARKER = 0x41E00000
JR_RA = 0x03E00008
IMPORT_ORDINAL = 0x24000000


def scan_irx_imports(data):
    if (len(data) < 52 or len(data) > MAX_IRX_BYTES or
            data[:6] != b"\x7fELF\x01\x01" or
            int.from_bytes(data[18:20], "little") != 8):
        raise ValueError("invalid ELF32 little-endian MIPS IRX")

    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    if phnum > MAX_PROGRAM_HEADERS or (phnum and phentsize < 32):
        raise ValueError("invalid IRX program-header table")
    if phnum and (phoff > len(data) or
                  phnum * phentsize > len(data) - phoff):
        raise ValueError("truncated IRX program-header table")

    imports = set()
    for i in range(phnum):
        ph = phoff + i * phentsize
        segment_type, start = struct.unpack_from("<II", data, ph)
        file_size = struct.unpack_from("<I", data, ph + 16)[0]
        if segment_type != 1 or file_size == 0:
            continue
        if start > len(data) or file_size > len(data) - start:
            raise ValueError("PT_LOAD segment exceeds IRX file bounds")
        end = start + file_size
        aligned_start = (start + 3) & ~3
        for offset in range(aligned_start, max(aligned_start, end - 19), 4):
            if struct.unpack_from("<I", data, offset)[0] != IMPORT_MARKER:
                continue
            raw_name = data[offset + 12:offset + 20]
            raw_name = raw_name.split(b"\x00", 1)[0]
            if (not raw_name or
                    any(not (48 <= ch <= 57 or 65 <= ch <= 90 or
                             97 <= ch <= 122 or ch == 95)
                        for ch in raw_name)):
                continue
            library = raw_name.decode("ascii")
            stubs = []
            stub = offset + 20
            while stub + 8 <= end:
                first, second = struct.unpack_from("<II", data, stub)
                if first != JR_RA or second & 0xFFFF0000 != IMPORT_ORDINAL:
                    break
                stubs.append(second & 0xFFFF)
                stub += 8
            if not stubs:
                continue
            for ordinal in stubs:
                imports.add((library, ordinal))
                if len(imports) > MAX_IMPORTS:
                    raise ValueError("IRX import count exceeded safety limit")

    return [
        {"library": library, "ordinal": ordinal}
        for library, ordinal in sorted(imports)
    ]
