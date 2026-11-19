#!/usr/bin/env python3
"""Synthetic file-backed IRX import inventory and malformed-input checks."""

from pathlib import Path
import struct
import sys

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
from iop_irx_imports import scan_irx_imports


def image():
    raw = bytearray(0x320)
    raw[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", raw, 16, 0xFF80, 8, 1)
    struct.pack_into("<I", raw, 28, 0x34)
    struct.pack_into("<HH", raw, 42, 32, 1)
    struct.pack_into("<IIIIIIII", raw, 0x34,
                     1, 0x100, 0, 0, 0x180, 0x180, 5, 4)
    return raw


def table(raw, start, library, ordinals):
    struct.pack_into("<I", raw, start, 0x41E00000)
    raw[start + 12:start + 20] = library.encode("ascii").ljust(8, b"\x00")
    for i, ordinal in enumerate(ordinals):
        struct.pack_into("<II", raw, start + 20 + i * 8,
                         0x03E00008, 0x24000000 | ordinal)


def expect_error(data, message):
    try:
        scan_irx_imports(data)
    except ValueError as exc:
        assert message in str(exc), (message, exc)
    else:
        raise AssertionError(f"expected rejection: {message}")


def main():
    raw = image()
    table(raw, 0x120, "sysclib", [12, 13, 29])
    table(raw, 0x170, "loadcore", [6, 7])
    # Outside file-backed PT_LOAD; should not be scanned as executable data.
    table(raw, 0x290, "fakesvc", [999])
    expected = [
        {"library": "loadcore", "ordinal": 6},
        {"library": "loadcore", "ordinal": 7},
        {"library": "sysclib", "ordinal": 12},
        {"library": "sysclib", "ordinal": 13},
        {"library": "sysclib", "ordinal": 29},
    ]
    assert scan_irx_imports(raw) == expected

    empty = image()
    assert scan_irx_imports(empty) == []
    bad_name = image()
    table(bad_name, 0x120, "sysclib", [12])
    bad_name[0x120 + 12] = ord('$')
    assert scan_irx_imports(bad_name) == []
    no_stubs = image()
    struct.pack_into("<I", no_stubs, 0x120, 0x41E00000)
    no_stubs[0x12C:0x134] = b"sysclib\x00"
    assert scan_irx_imports(no_stubs) == []

    invalid = image()
    invalid[18:20] = b"\x03\x00"  # x86 machine
    expect_error(invalid, "MIPS")
    truncated = image()
    struct.pack_into("<I", truncated, 0x34 + 16, 0x10000)
    expect_error(truncated, "PT_LOAD")
    bad_ph = image()
    struct.pack_into("<I", bad_ph, 28, 0xFFFFFFF0)
    expect_error(bad_ph, "program-header")
    too_many = image()
    struct.pack_into("<H", too_many, 44, 65)
    expect_error(too_many, "program-header")
    expect_error(b"not an ELF", "invalid ELF32")

    print("iop_irx_imports: PASS")


if __name__ == "__main__":
    main()
