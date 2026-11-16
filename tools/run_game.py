#!/usr/bin/env python3
"""Build and execute a bounded slice of a PS2 EE ELF through native code.

ee_recomp -> LLVM IR -> host objects -> link_native_objects -> ee_game_runner.
No BIOS, IOP, GS, DMA or general MMIO emulation is implied.

Examples:
  python tools/run_game.py SLUS_214.42 --build-dir build --trace
  python tools/run_game.py SLUS_214.42 --hle-startup --auto-expand=16 --trace
  python tools/run_game.py SLUS_214.42 --extra-functions=0x17f018,0x17edc8
  python tools/run_game.py SLUS_214.42 --functions=all --work-dir=build/sdbz
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys


STOP_PATTERN = re.compile(
    r"EE stopped: dispatched=(\d+) pc=0x([0-9a-fA-F]{8}) "
    r"reason=DISPATCH\(3\) status=0"
)



CACHE_SCHEMA = "ee-translation-cache-v1"


def sha256_path(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def translation_inputs(elf, recomp, source_root):
    """Fingerprint the ELF and every local input that builds translated code."""
    paths = [
        ("ELF", elf),
        ("ee_recomp", recomp),
        ("tools/run_game.py", source_root / "tools" / "run_game.py"),
        ("tools/verify_ir.py", source_root / "tools" / "verify_ir.py"),
        ("tools/link_native_objects.py",
         source_root / "tools" / "link_native_objects.py"),
        ("src/ee/ee_dispatcher.cpp",
         source_root / "src" / "ee" / "ee_dispatcher.cpp"),
        ("src/ee/recompiled_functions.cpp",
         source_root / "src" / "ee" / "recompiled_functions.cpp"),
    ]
    for relative in ("include/ee", "src/runtime"):
        directory = source_root / relative
        for path in sorted(directory.rglob("*")):
            if path.is_file() and path.suffix in (".h", ".hpp", ".cpp"):
                paths.append((path.relative_to(source_root).as_posix(), path))
    return {name: sha256_path(path) for name, path in paths}


def write_translation_cache(path, inputs, emit_opt, metadata_path, library):
    """Commit provenance only after the complete translated library is linked."""
    snapshot = {
        "schema": CACHE_SCHEMA,
        "inputs": inputs,
        "emit_opt": emit_opt,
        "metadata_sha256": sha256_path(metadata_path),
        "library_sha256": sha256_path(library),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text(json.dumps(snapshot, indent=2) + "\n",
                             encoding="utf-8")
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def validate_translation_cache(path, inputs, emit_opt, metadata_path, library):
    """Never reuse a translated DLL whose guest code provenance is unknown."""
    if not path.is_file():
        raise ValueError(
            "cached translation has no provenance record; "
            "rerun without --no-recompile"
        )
    snapshot = json.loads(path.read_text(encoding="utf-8"))
    if snapshot.get("schema") != CACHE_SCHEMA:
        raise ValueError(
            "cached translation has an unrecognized provenance schema; "
            "rerun without --no-recompile"
        )
    saved = snapshot.get("inputs")
    if not isinstance(saved, dict):
        raise ValueError("cached translation has invalid input fingerprints")
    for name, digest in inputs.items():
        if saved.get(name) != digest:
            raise ValueError(
                f"cached translation is stale: {name} changed; "
                "rerun without --no-recompile"
            )
    if set(saved) != set(inputs):
        raise ValueError(
            "cached translation input list changed; "
            "rerun without --no-recompile"
        )
    if snapshot.get("emit_opt") != emit_opt:
        raise ValueError(
            "cached translation uses a different --emit-opt; "
            "rerun without --no-recompile"
        )
    for name, file_path in (("metadata_sha256", metadata_path),
                            ("library_sha256", library)):
        if snapshot.get(name) != sha256_path(file_path):
            raise ValueError(
                f"cached translation {file_path.name} changed; "
                "rerun without --no-recompile"
            )


def elf_entry(path):
    with path.open("rb") as source:
        header = source.read(52)
    if len(header) < 52 or header[:4] != b"\x7fELF" or header[4] != 1:
        raise ValueError(f"{path} is not ELF32")
    if header[5] != 1 or struct.unpack_from("<H", header, 18)[0] != 8:
        raise ValueError(f"{path} is not a little-endian MIPS ELF")
    return struct.unpack_from("<I", header, 24)[0]


def executable_segments(path):
    image = path.read_bytes()
    if len(image) < 52:
        raise ValueError("truncated ELF32 header")
    program_offset = struct.unpack_from("<I", image, 28)[0]
    program_size = struct.unpack_from("<H", image, 42)[0]
    program_count = struct.unpack_from("<H", image, 44)[0]
    if program_size < 32 or program_offset + program_size * program_count > len(image):
        raise ValueError("invalid ELF32 program header table")

    ranges = []
    for index in range(program_count):
        offset = program_offset + program_size * index
        kind, _, va, _, _, memory_size, flags, _ = struct.unpack_from(
            "<IIIIIIII", image, offset
        )
        if kind == 1 and flags & 1 and memory_size > 0:
            ranges.append((va, va + memory_size))
    return ranges


def parse_address(value):
    number = int(value, 0)
    if not 0 <= number <= 0xFFFFFFFF or number & 3:
        raise ValueError(f"unaligned or out-of-range guest PC: {value}")
    return number


def run(command, capture=False, timeout_seconds=None):
    print("+ " + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    result = subprocess.run(
        [str(item) for item in command],
        text=True,
        capture_output=capture,
        check=not capture,
        timeout=timeout_seconds,
    )
    if capture:
        if result.stdout:
            print(result.stdout, end="", flush=True)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr, flush=True)
    return result


def native_executable(build_dir, name, config="Release"):
    suffix = ".exe" if os.name == "nt" else ""
    cache = build_dir / "CMakeCache.txt"
    multiconfig = False
    if cache.is_file():
        multiconfig = any(
            line.startswith("CMAKE_CONFIGURATION_TYPES:")
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines()
        )

    # Visual Studio and other multi-config generators write to
    # build/Release (or build/Debug), not the build directory itself.
    # Never accidentally use an older root executable in that case.
    executable = (build_dir / config / (name + suffix) if multiconfig
                  else build_dir / (name + suffix))
    if not executable.is_file():
        raise ValueError(
            f"required build target not found: {executable}\n"
            f"Run cmake --build {build_dir} --config {config} "
            "--target ee_recomp ee_game_runner"
        )
    return executable


def covered_pcs(metadata):
    return {
        int(pc, 0)
        for item in metadata.get("functions", [])
        for pc in item.get("block_pcs", [])
    }


def append_function(recomp, elf, work, metadata_path, address):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if address in covered_pcs(metadata):
        return False

    output_dir = work / "extra" / f"{address:08x}"
    command = [recomp, elf, output_dir, f"--function=0x{address:08x}"]
    discovery_cache = work / "ir" / "ee_discovery_cache.bin"
    if discovery_cache.is_file():
        command.append(f"--discovery-cache={discovery_cache}")
    run(command)
    extra_metadata = json.loads((output_dir / "ee_functions.json").read_text(
        encoding="utf-8"
    ))
    extra_functions = extra_metadata.get("functions", [])
    if len(extra_functions) != 1:
        raise ValueError(f"no unique translation for 0x{address:08x}")

    function = extra_functions[0]
    new_pcs = {int(pc, 0) for pc in function["block_pcs"]}
    if address not in new_pcs:
        raise ValueError(f"translation does not cover 0x{address:08x}")
    # One guest instruction can be entered through multiple native
    # functions. In particular, a call may land on an instruction inside
    # an existing basic block (e.g. a syscall wrapper's SYSCALL instruction).
    # Each separately lifted function can execute its own copy of shared
    # successor blocks, but the dispatcher must assign *one* native owner
    # to each guest block PC. Retain ownership for previously compiled
    # entries and publish only previously uncovered entry PCs.
    #
    # Never edit the generated LLVM IR or skip overlapping instructions:
    # internal branches still execute their independently lifted blocks.
    overlap = new_pcs & covered_pcs(metadata)
    if overlap:
        function["block_pcs"] = [
            pc for pc in function["block_pcs"] if int(pc, 0) not in overlap
        ]
        if not function["block_pcs"] or address not in {
            int(pc, 0) for pc in function["block_pcs"]
        }:
            raise ValueError(
                f"new entry 0x{address:08x} is already owned by "
                "an existing translated function"
            )
        print(
            f"auto-expand: preserving existing ownership of "
            f"{len(overlap)} overlapping guest block PC(s) "
            f"while translating entry 0x{address:08x}",
            flush=True,
        )

    destination = work / "ir" / Path(function["ir_path"]).name
    if destination.exists():
        raise ValueError(f"IR module filename collision: {destination}")
    shutil.copyfile(Path(function["ir_path"]), destination)
    function["ir_path"] = str(destination)
    metadata["functions"].append(function)
    metadata["function_count"] = len(metadata["functions"])
    metadata_path.write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    return True


def build_linked_library(source_root, work, metadata_path, object_dir, library,
                         emit_opt, pdb=False, reuse_objects=False, emit_jobs=2):
    if not reuse_objects:
        run([
        sys.executable, source_root / "tools" / "verify_ir.py",
        str(work / "ir" / "*.ll"),
        f"--emit-dir={object_dir}",
        f"--emit-opt={emit_opt}",
        "--incremental",
        f"--jobs={emit_jobs}",
        ])
    command = [
        sys.executable, source_root / "tools" / "link_native_objects.py",
        metadata_path, object_dir, library,
        "--source-root", source_root,
    ]
    if pdb:
        command.append("--pdb")
    run(command)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"),
                        help="CMake configuration for multi-config generators (default: Release)")
    parser.add_argument("--work-dir", type=Path, default=Path("build/game-run"))
    parser.add_argument(
        "--functions", default="entry",
        help="'entry' (default), a positive function cap, or 'all'",
    )
    parser.add_argument(
        "--extra-functions", default="",
        help="comma-separated additional guest function PCs, e.g. 0x17f018",
    )
    parser.add_argument(
        "--auto-expand", type=int, default=0,
        help="relink/replay with up to N newly reached executable function PCs",
    )
    parser.add_argument("--pdb", action="store_true",
                        help="Generate game-translated.pdb using Windows Clang/LLD")
    parser.add_argument("--emit-opt", type=int, choices=range(4), default=0,
                        metavar="{0,1,2,3}",
                        help="LLVM native object optimization level (default: 0)")
    parser.add_argument("--emit-jobs", type=int, default=2,
                        help="parallel LLVM object-emission processes (default: 2)")
    parser.add_argument("--max-dispatches", type=int, default=10000)
    parser.add_argument(
        "--loop-budget", type=int, default=0,
        help="Optional translated CFG loop-header watchdog limit per native dispatch (0=off)",
    )
    parser.add_argument(
        "--timeout-seconds", type=int, default=10,
        help="hard wall-time limit for each guest replay (default: 10)",
    )
    parser.add_argument("--entry", type=lambda value: int(value, 0))
    parser.add_argument("--stack", type=lambda value: int(value, 0))
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--trace-entry", type=lambda value: int(value, 0),
                        help="Dump guest arguments and backed pointer words on entry to a specified PC")
    parser.add_argument("--watch-word", type=lambda value: int(value, 0),
                        help="Log changes to a 32-bit guest RAM word between native dispatches")
    parser.add_argument("--report", type=Path,
                        help="Save structured EE stop state and last 64 native dispatches as JSON")
    parser.add_argument("--kernel-ram", type=Path,
                        help="512 KiB raw EE kernel RAM snapshot captured at game entry (not BIOS ROM)")
    parser.add_argument("--kseg-ram", action="store_true",
                        help="Map EE KSEG0/KSEG1 RAM mirrors (not MMIO or TLB)")
    parser.add_argument("--hle-startup", action="store_true",
                        help="Experimental SetupThread and SetupHeap HLE")
    parser.add_argument("--no-recompile", action="store_true",
                        help="Reuse existing metadata and linked library; --auto-expand may still add functions")
    parser.add_argument("--link-only", action="store_true",
                        help="Stop after generating/linking the native DLL (and PDB if requested)")
    args = parser.parse_args(argv)

    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be between 1 and 32")
    if args.max_dispatches <= 0 or args.max_dispatches > 0xFFFFFFFF:
        parser.error("--max-dispatches must fit a positive uint32")
    if args.loop_budget < 0 or args.loop_budget > 0xFFFFFFFF:
        parser.error("--loop-budget must fit a uint32")
    for name in ("trace_entry", "watch_word"):
        address = getattr(args, name)
        if address is not None and not (0 <= address <= 0xFFFFFFFF and address % 4 == 0):
            parser.error(f"--{name.replace('_', '-')} must be an aligned uint32")
    if args.timeout_seconds < 1 or args.timeout_seconds > 3600:
        parser.error("--timeout-seconds must be in [1, 3600]")
    if args.auto_expand < 0 or args.auto_expand > 128:
        parser.error("--auto-expand must be between 0 and 128")
    if args.pdb and os.name != "nt":
        parser.error("--pdb requires a Windows Clang/LLD host toolchain")
    if args.no_recompile and args.extra_functions:
        parser.error("--no-recompile cannot combine with --extra-functions")
    if args.link_only and args.auto_expand:
        parser.error("--link-only cannot be combined with --auto-expand")
    try:
        functions = args.functions
        if functions not in ("entry", "all") and (
            not functions.isdecimal() or int(functions) <= 0 or int(functions) > 0xFFFFFFFF
        ):
            parser.error("--functions must be entry, all or a positive integer")

        extra = [
            parse_address(item.strip())
            for item in args.extra_functions.split(",") if item.strip()
        ]
        elf = args.elf.resolve(strict=True)
        build = args.build_dir.resolve()
        work = args.work_dir.resolve()
        entry = elf_entry(elf) if args.entry is None else args.entry
        if not 0 <= entry <= 0xFFFFFFFF or entry & 3:
            parser.error("--entry must be an aligned uint32")
        if args.stack is not None and not 0 <= args.stack <= 0xFFFFFFFF:
            parser.error("--stack must fit uint32")

        source_root = Path(__file__).resolve().parents[1]
        runner = native_executable(build, "ee_game_runner", args.config)
        metadata_path = work / "ir" / "ee_functions.json"
        object_dir = work / "objects"
        library_name = "game-translated.dll" if os.name == "nt" else (
            "libgame-translated.dylib" if sys.platform == "darwin" else
            "libgame-translated.so"
        )
        library = work / library_name
        recomp = native_executable(build, "ee_recomp", args.config)
        cache_path = work / "translation-cache.json"
        inputs = translation_inputs(elf, recomp, source_root)

        if not args.no_recompile:
            (work / "ir").mkdir(parents=True, exist_ok=True)
            for stale in (work / "ir").glob("*.ll"):
                stale.unlink()

            command = [recomp, elf, work / "ir"]
            if functions == "entry":
                command.append(f"--function=0x{entry:08x}")
            elif functions != "all":
                command.append(f"--max-functions={functions}")
            run(command)

            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            if entry not in covered_pcs(metadata):
                raise ValueError(
                    f"ELF entry 0x{entry:08x} is not compiled. "
                    "Use --functions=entry or --functions=all."
                )

            for address in dict.fromkeys(extra):
                append_function(recomp, elf, work, metadata_path, address)
            build_linked_library(
                source_root, work, metadata_path, object_dir, library,
                args.emit_opt, pdb=args.pdb, emit_jobs=args.emit_jobs
            )
            write_translation_cache(cache_path, inputs, args.emit_opt,
                                    metadata_path, library)
        else:
            if not library.is_file() or not metadata_path.is_file():
                raise ValueError(
                    f"--no-recompile needs an existing linked library and metadata: "
                    f"{library}, {metadata_path}"
                )
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            source_elf = Path(metadata.get("elf", "")).resolve()
            if source_elf != elf:
                raise ValueError(
                    "--no-recompile ELF does not match existing translation: "
                    f"{source_elf} != {elf}"
                )
            if entry not in covered_pcs(metadata):
                raise ValueError(
                    f"--no-recompile entry 0x{entry:08x} has no translated block"
                )
            validate_translation_cache(cache_path, inputs, args.emit_opt,
                                       metadata_path, library)
            if args.pdb:
                build_linked_library(
                    source_root, work, metadata_path, object_dir, library,
                    args.emit_opt, pdb=True, reuse_objects=True, emit_jobs=args.emit_jobs
                )
                write_translation_cache(cache_path, inputs, args.emit_opt,
                                        metadata_path, library)
            if args.auto_expand:
                recomp = native_executable(build, "ee_recomp", args.config)
                if not object_dir.is_dir():
                    raise ValueError(
                        f"--no-recompile --auto-expand requires object directory: {object_dir}"
                    )

        if args.link_only:
            print(f"run_game: native library ready: {library}", flush=True)
            if args.pdb:
                print(f"run_game: PDB ready: {library.with_suffix('.pdb')}", flush=True)
            return 0

        command = [
            runner, elf, library,
            f"--max-dispatches={args.max_dispatches}",
            f"--loop-budget={args.loop_budget}",
            f"--entry=0x{entry:08x}",
        ]
        if args.stack is not None:
            command.append(f"--stack=0x{args.stack:08x}")
        if args.trace:
            command.append("--trace")
        if args.trace_entry is not None:
            command.append(f"--trace-entry=0x{args.trace_entry:08x}")
        if args.watch_word is not None:
            command.append(f"--watch-word=0x{args.watch_word:08x}")
        if args.report is not None:
            report_path = args.report.resolve()
            report_path.parent.mkdir(parents=True, exist_ok=True)
            command.append(f"--report={report_path}")
        if args.hle_startup:
            command.append("--hle-startup")
        if args.kseg_ram:
            command.append("--kseg-ram")
        if args.kernel_ram is not None:
            if not args.kseg_ram:
                parser.error("--kernel-ram requires --kseg-ram")
            snapshot = args.kernel_ram.resolve(strict=True)
            if snapshot.stat().st_size != 0x80000:
                raise ValueError(
                    f"EE kernel RAM snapshot must be exactly 524288 bytes: {snapshot}"
                )
            with snapshot.open("rb") as snapshot_file:
                signature = snapshot_file.read(4)
            if signature in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08"):
                raise ValueError(
                    "kernel RAM snapshot contains ZIP/savestate bytes, not raw EE RAM; "
                    "extract and decompress the internal eeMemory.bin member first"
                )
            command.append(f"--kernel-ram={snapshot}")

        ranges = executable_segments(elf) if args.auto_expand else []
        for replay in range(args.auto_expand + 1):
            print(
                f"\n== Executing translated EE code (replay {replay}) ==",
                flush=True,
            )
            try:
                result = run(
                    command, capture=True,
                    timeout_seconds=args.timeout_seconds,
                )
            except subprocess.TimeoutExpired as error:
                if error.stdout:
                    print(
                        error.stdout.decode(errors="replace")
                        if isinstance(error.stdout, bytes) else error.stdout,
                        end="",
                    )
                if error.stderr:
                    print(
                        error.stderr.decode(errors="replace")
                        if isinstance(error.stderr, bytes) else error.stderr,
                        end="", file=sys.stderr,
                    )
                print(
                    "run_game: replay exceeded its wall-clock timeout; "
                    "this may be slow tracing, guest polling, or an internal loop",
                    file=sys.stderr,
                )
                if args.report is not None and report_path.is_file():
                    try:
                        snapshot = json.loads(report_path.read_text(encoding="utf-8"))
                        recent = snapshot.get("history", [])
                        latest = recent[-1] if recent else None
                        print(
                            "run_game: last saved snapshot "
                            f"kind={snapshot.get('snapshot_kind', 'unknown')} "
                            f"dispatches={snapshot.get('dispatches')} "
                            f"pc=0x{snapshot['final_pc']:08x} "
                            f"replay={replay}",
                            file=sys.stderr,
                        )
                        if latest:
                            print(
                                "run_game: last recorded dispatch "
                                f"pc=0x{latest['from_pc']:08x} -> "
                                f"0x{latest['to_pc']:08x} "
                                f"reason={latest['reason']}",
                                file=sys.stderr,
                            )
                    except (OSError, ValueError, KeyError, TypeError) as snapshot_error:
                        print(
                            f"run_game: could not read timeout snapshot: {snapshot_error}",
                            file=sys.stderr,
                        )
                return 124
            if replay == args.auto_expand or result.returncode not in (3, 4):
                return result.returncode

            match = STOP_PATTERN.search(result.stdout)
            if not match or int(match.group(1)) == 0:
                return result.returncode
            address = int(match.group(2), 16)
            if not any(start <= address < end for start, end in ranges):
                print(
                    f"auto-expand stopped at non-executable PC 0x{address:08x}",
                    file=sys.stderr,
                )
                return result.returncode
            if address in covered_pcs(
                json.loads(metadata_path.read_text(encoding="utf-8"))
            ):
                print(
                    f"auto-expand stopped at already-compiled PC 0x{address:08x}",
                    file=sys.stderr,
                )
                return result.returncode

            print(
                f"auto-expand: compiling reachable PC 0x{address:08x}",
                flush=True,
            )
            if not append_function(recomp, elf, work, metadata_path, address):
                return result.returncode
            build_linked_library(
                source_root, work, metadata_path, object_dir, library,
                args.emit_opt, pdb=args.pdb, emit_jobs=args.emit_jobs
            )
            write_translation_cache(cache_path, inputs, args.emit_opt,
                                    metadata_path, library)
        return 3
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"run_game: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
