#!/usr/bin/env python3
"""Regression tests for PS2 disc IRX path resolution and startup safety."""

import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "iop_disc_catalog", TOOLS / "iop_disc_catalog.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_mips_elf(path):
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", header, 18, 8)
    path.write_bytes(header)


def make_test_elf(paths):
    image = bytearray(0x100 + 0x500)
    image[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", image, 16, 2, 8, 1)
    struct.pack_into("<I", image, 28, 52)
    struct.pack_into("<HH", image, 40, 52, 32)
    struct.pack_into("<H", image, 44, 1)
    struct.pack_into("<IIIIIIII", image, 52,
                     1, 0x100, 0x100000, 0x100000, 0x500, 0x500, 5, 4)
    at = 0x160
    for path in paths:
        encoded = path.encode("ascii") + b"\x00"
        image[at:at + len(encoded)] = encoded
        at += len(encoded) + 8
    return image


def expect_error(callback, fragment):
    try:
        callback()
    except ValueError as error:
        assert fragment in str(error), (fragment, error)
    else:
        raise AssertionError(f"expected failure containing {fragment!r}")


def main():
    assert MODULE.parse_cdrom_path(r"cdrom0:\PADMAN.IRX;1") == (
        "PADMAN.IRX;1",
    )
    assert MODULE.parse_cdrom_path("cdrom0:/DATA/ARIKA.IRX") == (
        "DATA", "ARIKA.IRX",
    )
    for guest in (
        "rom0:PADMAN.IRX",
        "cdrom0:/../secret.irx",
        "cdrom0:/DIR/../../secret.irx",
        "cdrom0:C:/system.irx",
        "cdrom0:/PADMAN.DAT",
        "cdrom0:/a//mod.irx",
        "cdrom0:/a/\nmod.irx",
    ):
        assert MODULE.parse_cdrom_path(guest) is None, guest

    with tempfile.TemporaryDirectory(prefix="iop-disc-catalog-") as temp:
        root = Path(temp)
        disc = root / "PS2_GAME"
        disc.mkdir()
        (disc / "DATA").mkdir()
        write_mips_elf(disc / "padman.irx")
        write_mips_elf(disc / "DATA" / "ArIkA.IrX;1")
        elf = disc / "SLUS_214.42"
        elf.write_bytes(make_test_elf([
            r"cdrom0:\PADMAN.IRX;1",
            "cdrom0:/DATA/ARIKA.IRX;1",
            r"cdrom0:\MISSING.IRX;1",
            "rom0:LOADCORE.IRX",
        ]))
        result = MODULE.build_catalog(elf)
        assert result["schema"] == "iop-disc-catalog-v1"
        assert [item["status"] for item in result["candidates"]] == [
            "resolved", "resolved", "missing", "unsupported-device",
        ], result
        assert result["resolved_count"] == 2
        assert result["runtime_load_count"] == 0
        assert result["candidates"][0]["host_path"] == str(
            disc / "padman.irx"
        )
        assert result["candidates"][1]["host_path"] == str(
            disc / "DATA" / "ArIkA.IrX;1"
        )

        output = root / "catalog.json"
        proc = subprocess.run([
            sys.executable, str(TOOLS / "iop_disc_catalog.py"),
            str(elf), "--json", str(output)
        ], text=True, capture_output=True, check=False)
        assert proc.returncode == 0, proc.stderr
        assert "no modules loaded or executed" in proc.stdout
        assert json.loads(output.read_text())["resolved_count"] == 2

        # Case-insensitive ambiguity is an error even on Linux.
        write_mips_elf(disc / "PADMAN.IRX;1")
        expect_error(
            lambda: MODULE.build_catalog(elf), "ambiguous"
        )
        (disc / "PADMAN.IRX;1").unlink()

        # A symlink inside the disc cannot be used to access host files.
        (disc / "padman.irx").unlink()
        try:
            (disc / "padman.irx").symlink_to(
                root / "secret.irx"
            )
        except (OSError, NotImplementedError):
            pass
        else:
            expect_error(
                lambda: MODULE.build_catalog(elf), "symlink"
            )
            (disc / "padman.irx").unlink()

        # A file with an .IRX name is not necessarily an IRX.
        (disc / "padman.irx").write_bytes(b"not an IRX" * 8)
        expect_error(
            lambda: MODULE.build_catalog(elf), "MIPS IRX"
        )

    print("IOP disc candidate resolution: PASS")


if __name__ == "__main__":
    main()
