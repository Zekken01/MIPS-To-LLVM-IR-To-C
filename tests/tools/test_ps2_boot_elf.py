#!/usr/bin/env python3
"""Redistributable fixtures for PS2_GAME SYSTEM.CNF BOOT2 lookup."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
from resolve_ps2_boot import resolve_boot_elf  # noqa: E402


def expect_failure(root, fragment):
    try:
        resolve_boot_elf(root)
    except (ValueError, OSError) as error:
        assert fragment in str(error), (fragment, str(error))
    else:
        raise AssertionError(f"expected failure containing {fragment!r}")


def main():
    with tempfile.TemporaryDirectory(prefix="system-cnf-") as tmp:
        root = Path(tmp) / "PS2 GAME with spaces"
        (root / "SubDir").mkdir(parents=True)
        elf = root / "SubDir" / "Slus_214.42"
        elf.write_bytes(b"fake test ELF placeholder")
        cnf = root / "system.cnf"
        cnf.write_bytes(
            b"BOOT2 = cdrom0:\\SUBDIR\\SLUS_214.42;1\r\n"
            b"VER = 1.00\r\nVMODE = NTSC\r\n"
        )

        assert resolve_boot_elf(root) == elf
        run = subprocess.run(
            [sys.executable, str(SOURCE / "tools" / "resolve_ps2_boot.py"),
             str(root)],
            capture_output=True, text=True, check=False,
        )
        assert run.returncode == 0, run.stderr
        assert run.stdout.strip() == str(elf), run.stdout
        assert run.stderr == "", run.stderr

        cnf.write_text('  boot2  =  "cdrom0:/subdir/slus_214.42;1"  \n',
                       encoding="ascii")
        assert resolve_boot_elf(root) == elf

        # A preserved ISO9660 version suffix is also accepted, but if an
        # extractor provides both forms the choice must be rejected.
        versioned = root / "SubDir" / "Slus_214.42;1"
        elf.rename(versioned)
        assert resolve_boot_elf(root) == versioned
        elf.write_bytes(b"duplicate extracted version")
        expect_failure(root, "ambiguous")
        elf.unlink()

        cnf.write_text("BOOT2 = cdrom0:/SubDir/../SYSTEM.CNF;1\n",
                       encoding="ascii")
        expect_failure(root, "unsafe")
        cnf.write_text("BOOT2 = host:/SubDir/Slus_214.42\n",
                       encoding="ascii")
        expect_failure(root, "cdrom0")
        cnf.write_text("BOOT2 = cdrom0:/SubDir/Slus_214.42;x\n",
                       encoding="ascii")
        expect_failure(root, "version")
        cnf.write_text("BOOT2 = cdrom0:/SubDir/Missing.ELF;1\n",
                       encoding="ascii")
        expect_failure(root, "not found")
        cnf.write_text("BOOT2 = cdrom0:/SubDir/Slus_214.42;1\n"
                       "BOOT2 = cdrom0:/SubDir/Slus_214.42;1\n",
                       encoding="ascii")
        expect_failure(root, "exactly one")
        cnf.write_text("VER = 1.00\n", encoding="ascii")
        expect_failure(root, "exactly one")

        # Reject a symlink to a file outside PS2_GAME, even when its
        # filename matches the guest request. Works on POSIX and on
        # Windows where developer-mode symlink privileges are available.
        cnf.write_text("BOOT2 = cdrom0:/SubDir/EXTERNAL.ELF;1\n",
                       encoding="ascii")
        outside = Path(tmp) / "EXTERNAL.ELF"
        outside.write_bytes(b"external fixture")
        symlink = root / "SubDir" / "EXTERNAL.ELF"
        try:
            symlink.symlink_to(outside)
        except (NotImplementedError, OSError):
            pass
        else:
            expect_failure(root, "symlinks")

        cnf.write_text("BOOT2 = cdrom0:/SubDir/Slus_214.42;1\n",
                       encoding="ascii")
        symlink.unlink(missing_ok=True)
        # A case-insensitive duplicate of SYSTEM.CNF is invalid when the
        # host filesystem supports distinct names.
        second_cnf = root / "SYSTEM.CNF"
        if not second_cnf.exists():
            second_cnf.write_text(cnf.read_text(encoding="ascii"),
                                  encoding="ascii")
            expect_failure(root, "ambiguous")

        # The utility is intentionally read-only; it does not manufacture
        # a missing game ELF or copy/rename a versioned source.
        assert versioned.is_file()

    print("PS2 SYSTEM.CNF BOOT2 resolver: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
