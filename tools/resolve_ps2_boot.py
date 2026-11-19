#!/usr/bin/env python3
"""Resolve the BOOT2 ELF named by an extracted PS2 disc's SYSTEM.CNF.

Print only the absolute host ELF path on success, for use by Windows batch
launchers. Never infer a region or executable from a filename convention.
"""
import argparse
from pathlib import Path
import re


_BOOT_DEVICE = re.compile(r"^cdrom0:[\\/]*(.+)$", re.IGNORECASE)
_VERSION = re.compile(r"^(.+);([0-9]+)$")


def _select_component(directory, names, guest_path):
    accepted = {name.casefold() for name in names}
    matches = [
        entry for entry in directory.iterdir()
        if entry.name.casefold() in accepted
    ]
    if len(matches) != 1:
        if matches:
            raise ValueError(f"ambiguous case-insensitive disc path: {guest_path}")
        raise ValueError(f"BOOT2 file not found in extracted disc: {guest_path}")
    if matches[0].is_symlink():
        raise ValueError(f"symlinks are not allowed in disc paths: {matches[0]}")
    return matches[0]


def resolve_boot_elf(disc_root):
    """Resolve a unique BOOT2 cdrom0: file, rejecting traversal and ambiguity."""
    root = Path(disc_root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"PS2_GAME is not a regular directory: {root}")
    root = root.resolve(strict=True)

    cnf = _select_component(root, ["SYSTEM.CNF"], "SYSTEM.CNF")
    if not cnf.is_file():
        raise ValueError(f"SYSTEM.CNF is not a regular file: {cnf}")
    try:
        content = cnf.read_text(encoding="ascii")
    except UnicodeError as error:
        raise ValueError(f"SYSTEM.CNF must be ASCII: {error}") from error

    entries = []
    for line in content.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip().upper() == "BOOT2":
            entries.append(value.strip())
    if len(entries) != 1 or not entries[0]:
        raise ValueError("SYSTEM.CNF must define exactly one nonempty BOOT2 entry")

    value = entries[0]
    if value.startswith('"') and value.endswith('"') and len(value) > 2:
        value = value[1:-1]
    match = _BOOT_DEVICE.fullmatch(value)
    if match is None:
        raise ValueError("SYSTEM.CNF BOOT2 must be a cdrom0: file path")
    guest_path = match.group(1).replace("\\", "/")
    parts = guest_path.split("/")
    if (len(parts) > 16 or
            any(not part or part in (".", "..") or len(part) > 128 or
                any(ord(ch) < 0x21 or ord(ch) > 0x7e or
                    ch in ':*?"<>|' for ch in part)
                for part in parts)):
        raise ValueError("SYSTEM.CNF BOOT2 contains an unsafe or invalid path")

    last = parts[-1]
    possible_last = [last]
    if ";" in last:
        version = _VERSION.fullmatch(last)
        if version is None:
            raise ValueError("SYSTEM.CNF BOOT2 contains an invalid ISO version")
        possible_last.append(version.group(1))

    current = root
    for index, part in enumerate(parts):
        current = _select_component(
            current,
            possible_last if index == len(parts) - 1 else [part],
            guest_path,
        )
        if index != len(parts) - 1 and not current.is_dir():
            raise ValueError(f"BOOT2 intermediate path is not a directory: {current}")
    if not current.is_file():
        raise ValueError(f"BOOT2 target is not a file: {current}")
    return current.resolve(strict=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_root", type=Path, nargs="?",
                        default=Path("PS2_GAME"))
    args = parser.parse_args(argv)
    try:
        elf = resolve_boot_elf(args.disc_root)
    except (OSError, ValueError) as error:
        parser.exit(1, f"PS2 boot resolution failed: {error}\n")
    print(elf)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
