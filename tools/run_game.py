#!/usr/bin/env python3
"""Build and execute a bounded slice of a PS2 EE ELF through native code.

The pipeline uses the existing ee_recomp -> LLVM IR -> host object ->
link_native_objects -> ee_game_runner stages. No PS2 BIOS, IOP, DMA,
GS or general MMIO emulation is implied.

Example:
  python tools/run_game.py SLUS_214.42 --build-dir build --trace
  python tools/run_game.py SLUS_214.42 --functions=128 --max-dispatches=1000
  python tools/run_game.py SLUS_214.42 --functions=all --work-dir=build/sdbz
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def elf_entry(path):
    with path.open("rb") as source:
        header = source.read(52)
    if len(header) < 52 or header[:4] != b"\x7fELF" or header[4] != 1:
        raise ValueError(f"{path} is not ELF32")
    if header[5] != 1 or struct.unpack_from("<H", header, 18)[0] != 8:
        raise ValueError(f"{path} is not a little-endian MIPS ELF")
    return struct.unpack_from("<I", header, 24)[0]


def run(command):
    print("+ " + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    subprocess.run([str(item) for item in command], check=True)


def native_executable(build_dir, name):
    suffix = ".exe" if os.name == "nt" else ""
    executable = build_dir / (name + suffix)
    if not executable.is_file():
        raise ValueError(
            f"required build target not found: {executable}\n"
            f"Run cmake --build {build_dir} --target ee_recomp ee_game_runner"
        )
    return executable


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--work-dir", type=Path, default=Path("build/game-run"))
    parser.add_argument(
        "--functions", default="entry",
        help="'entry' (default), a positive function cap, or 'all'",
    )
    parser.add_argument("--max-dispatches", type=int, default=10000)
    parser.add_argument("--entry", type=lambda value: int(value, 0))
    parser.add_argument("--stack", type=lambda value: int(value, 0))
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--hle-startup", action="store_true",
                        help="Experimentally handle SetupThread (0x3c) and resume")
    parser.add_argument("--no-recompile", action="store_true",
                        help="Reuse existing metadata, objects and linked library")
    args = parser.parse_args(argv)

    if args.max_dispatches <= 0 or args.max_dispatches > 0xFFFFFFFF:
        parser.error("--max-dispatches must fit a positive uint32")
    try:
        functions = args.functions
        if functions not in ("entry", "all") and (
            not functions.isdecimal() or int(functions) <= 0 or int(functions) > 0xFFFFFFFF
        ):
            parser.error("--functions must be entry, all or a positive integer")
        elf = args.elf.resolve(strict=True)
        build = args.build_dir.resolve()
        work = args.work_dir.resolve()
        entry = elf_entry(elf) if args.entry is None else args.entry
        if entry < 0 or entry > 0xFFFFFFFF:
            parser.error("--entry must fit uint32")
        if args.stack is not None and not 0 <= args.stack <= 0xFFFFFFFF:
            parser.error("--stack must fit uint32")

        source_root = Path(__file__).resolve().parents[1]
        runner = native_executable(build, "ee_game_runner")
        metadata_path = work / "ir" / "ee_functions.json"
        object_dir = work / "objects"
        library_name = "game-translated.dll" if os.name == "nt" else (
            "libgame-translated.dylib" if sys.platform == "darwin" else
            "libgame-translated.so"
        )
        library = work / library_name

        if not args.no_recompile:
            recomp = native_executable(build, "ee_recomp")
            work.mkdir(parents=True, exist_ok=True)
            command = [recomp, elf, work / "ir"]
            if functions == "entry":
                command.append(f"--function=0x{entry:08x}")
            elif functions != "all":
                command.append(f"--max-functions={functions}")
            run(command)

            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            if not any(
                entry in [int(pc, 0) for pc in item.get("block_pcs", [])]
                for item in metadata.get("functions", [])
            ):
                raise ValueError(
                    f"ELF entry 0x{entry:08x} is not in compiled function blocks. "
                    "Use --functions=entry or --functions=all."
                )

            run([
                sys.executable, source_root / "tools" / "verify_ir.py",
                str(work / "ir" / "*.ll"),
                f"--emit-dir={object_dir}",
                "--emit-opt=0",
            ])
            run([
                sys.executable, source_root / "tools" / "link_native_objects.py",
                metadata_path, object_dir, library,
                "--source-root", source_root,
            ])
        elif not library.is_file():
            raise ValueError(f"no linked library for --no-recompile: {library}")

        command = [
            runner, elf, library,
            f"--max-dispatches={args.max_dispatches}",
            f"--entry=0x{entry:08x}",
        ]
        if args.stack is not None:
            command.append(f"--stack=0x{args.stack:08x}")
        if args.trace:
            command.append("--trace")
        if args.hle_startup:
            command.append("--hle-startup")
        print("\n== Executing translated EE code ==", flush=True)
        run(command)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"run_game: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
