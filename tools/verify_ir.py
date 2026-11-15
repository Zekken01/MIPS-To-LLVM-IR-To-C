#!/usr/bin/env python3
"""Parse/verify generated LLVM IR and optionally emit native host objects.

Examples:
  python3 tools/verify_ir.py 'out/*.ll'
  python3 tools/verify_ir.py 'out/*.ll' --emit
  python3 tools/verify_ir.py 'out/*.ll' --emit-dir=out/objects --emit-opt=0
  python3 tools/verify_ir.py 'out/*.ll' --emit-dir=out/objects --emit-opt=0 --incremental --jobs=4

With --incremental, an existing object is reused only if its source IR
SHA-256, emitted object SHA-256, target triple, LLVM version and optimization
level all match. Each object and the complete manifest are written atomically.
Separate worker processes (not threads) provide safe concurrent LLVM emission.
"""
import collections
from concurrent.futures import ProcessPoolExecutor, as_completed
import glob
import hashlib
import json
import re
import sys
import time
from pathlib import Path

import llvmlite.binding as llvm


_worker_machines = {}


def write_atomic(path, data):
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(data)
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def get_target_machine(emit_opt, target_triple):
    key = (emit_opt, target_triple)
    if key not in _worker_machines:
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        target = llvm.Target.from_triple(target_triple)
        _worker_machines[key] = target.create_target_machine(
            opt=emit_opt, reloc="pic", codemodel="default"
        )
    return _worker_machines[key]


def compile_module(filename, emit_opt, emit_dir, target_triple):
    started = time.monotonic()
    try:
        ir_bytes = Path(filename).read_bytes()
        module = llvm.parse_assembly(ir_bytes.decode("utf-8"))
        module.verify()
        entry = None
        object_size = 0
        if emit_opt is not None:
            target_machine = get_target_machine(emit_opt, target_triple)
            # The MIPS/Windows-targeted guest IR must be emitted for the
            # current host, not the ELF's guest address-space layout.
            module.triple = target_triple
            module.data_layout = str(target_machine.target_data)
            module.verify()
            object_data = target_machine.emit_object(module)
            object_size = len(object_data)
            if emit_dir is not None:
                object_name = Path(filename).stem + ".o"
                write_atomic(Path(emit_dir) / object_name, object_data)
                entry = {
                    "input": str(Path(filename)),
                    "object": object_name,
                    "size_bytes": object_size,
                    "sha256": hashlib.sha256(object_data).hexdigest(),
                    "ir_sha256": hashlib.sha256(ir_bytes).hexdigest(),
                    "defined_symbols": [
                        fn.name for fn in module.functions if not fn.is_declaration
                    ],
                }
        return {
            "valid": True, "entry": entry, "object_size": object_size,
            "elapsed": time.monotonic() - started, "filename": filename,
        }
    except Exception as exc:
        return {
            "valid": False, "error": str(exc),
            "elapsed": time.monotonic() - started, "filename": filename,
        }


def cached_objects(emit_dir, emit_opt, target_triple, filenames):
    manifest_path = emit_dir / "objects.json"
    if not manifest_path.is_file():
        return {}
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (
            not manifest.get("complete")
            or manifest.get("optimization_level") != emit_opt
            or manifest.get("target_triple") != target_triple
            or manifest.get("llvm_version") != ".".join(map(str, llvm.llvm_version_info))
        ):
            return {}
        entries = manifest.get("objects")
        if not isinstance(entries, list) or len(entries) != len({
            item.get("input") for item in entries if isinstance(item, dict)
        }):
            return {}
        by_input = {item["input"]: item for item in entries}
        reused = {}
        for filename in filenames:
            item = by_input.get(filename)
            if not isinstance(item, dict):
                continue
            object_name = Path(filename).stem + ".o"
            if item.get("object") != object_name:
                continue
            if not isinstance(item.get("defined_symbols"), list):
                continue
            if item.get("ir_sha256") != hashlib.sha256(Path(filename).read_bytes()).hexdigest():
                continue
            output_path = emit_dir / object_name
            if not output_path.is_file():
                continue
            object_data = output_path.read_bytes()
            if item.get("size_bytes") != len(object_data):
                continue
            if item.get("sha256") != hashlib.sha256(object_data).hexdigest():
                continue
            reused[filename] = item
        return reused
    except (OSError, ValueError, KeyError, TypeError):
        # A malformed or old manifest is a cache miss, never trusted.
        return {}


def main():
    args = sys.argv[1:]
    if not args or "-h" in args or "--help" in args:
        print(__doc__)
        return 0 if args else 2

    emit_opt = None
    emit_dir = None
    incremental = False
    jobs = 1
    progress_every = 250
    patterns = []
    for arg in args:
        if arg == "--emit":
            emit_opt = 2
        elif arg.startswith("--emit-opt="):
            emit_opt = int(arg.split("=", 1)[1])
            if not 0 <= emit_opt <= 3:
                raise SystemExit("--emit-opt must be between 0 and 3")
        elif arg.startswith("--emit-dir="):
            emit_dir = Path(arg.split("=", 1)[1])
            if emit_opt is None:
                emit_opt = 0
        elif arg == "--incremental":
            incremental = True
        elif arg.startswith("--jobs="):
            jobs = int(arg.split("=", 1)[1])
            if not 1 <= jobs <= 32:
                raise SystemExit("--jobs must be between 1 and 32")
        elif arg.startswith("--progress="):
            progress_every = max(1, int(arg.split("=", 1)[1]))
        else:
            patterns.append(arg)
    if incremental and emit_dir is None:
        raise SystemExit("--incremental requires --emit-dir")

    files = sorted({str(Path(name)) for pattern in patterns for name in glob.glob(pattern)})
    if not files:
        print("No LLVM IR files matched the supplied pattern(s).")
        return 2

    target_triple = llvm.get_default_triple() if emit_opt is not None else None
    if emit_dir is not None:
        emit_dir.mkdir(parents=True, exist_ok=True)
        object_names = [Path(filename).stem + ".o" for filename in files]
        if len(set(object_names)) != len(object_names):
            raise SystemExit("input modules have duplicate basenames; refusing to overwrite objects")

    start = time.monotonic()
    reused = (cached_objects(emit_dir, emit_opt, target_triple, files)
              if incremental else {})
    pending = [filename for filename in files if filename not in reused]
    if emit_dir is not None:
        print(
            f"verify_ir: reused {len(reused)}/{len(files)} cached object(s); "
            f"emitting {len(pending)} with {min(jobs, max(1, len(pending)))} worker(s)",
            flush=True,
        )

    emitted = dict(reused)
    ok = len(reused)
    bad = 0
    object_bytes = sum(item["size_bytes"] for item in reused.values())
    errors = collections.Counter()
    first = {}
    slowest = (0.0, "")

    def record(result):
        nonlocal ok, bad, object_bytes, slowest
        if result["valid"]:
            ok += 1
            object_bytes += result["object_size"]
            if result["entry"] is not None:
                emitted[result["filename"]] = result["entry"]
        else:
            bad += 1
            lines = result["error"].strip().splitlines() or ["unknown error"]
            error_lines = [line for line in lines if "error:" in line]
            key = re.sub(
                r"\d+", "N",
                re.sub(r"%\w+", "%X", (error_lines[0] if error_lines else lines[0])),
            )[:110]
            errors[key] += 1
            first.setdefault(key, result["filename"])
        if result["elapsed"] > slowest[0]:
            slowest = (result["elapsed"], result["filename"])
        complete = ok + bad
        if complete % progress_every == 0 or complete == len(files):
            mode = f", object bytes {object_bytes:,}" if emit_opt is not None else ""
            print(
                f"[{complete}/{len(files)}] valid={ok} invalid={bad}{mode} "
                f"elapsed={time.monotonic() - start:.1f}s", flush=True,
            )

    if jobs > 1 and len(pending) > 1:
        with ProcessPoolExecutor(max_workers=min(jobs, len(pending))) as executor:
            futures = [
                executor.submit(compile_module, filename, emit_opt, emit_dir, target_triple)
                for filename in pending
            ]
            for future in as_completed(futures):
                record(future.result())
    else:
        for filename in pending:
            record(compile_module(filename, emit_opt, emit_dir, target_triple))

    if not pending:
        record_progress = f"[{ok}/{len(files)}] valid={ok} invalid={bad} elapsed={time.monotonic() - start:.1f}s"
        print(record_progress, flush=True)

    if emit_dir is not None:
        manifest = {
            "complete": bad == 0 and len(emitted) == len(files),
            "llvm_version": ".".join(map(str, llvm.llvm_version_info)),
            "target_triple": target_triple,
            "optimization_level": emit_opt,
            "input_module_count": len(files),
            "object_count": len(emitted),
            "object_bytes": sum(item["size_bytes"] for item in emitted.values()),
            "objects": [emitted[name] for name in files if name in emitted],
        }
        write_atomic(
            emit_dir / "objects.json",
            (json.dumps(manifest, indent=2) + "\n").encode("utf-8"),
        )

    print(
        f"LLVM {llvm.llvm_version_info}: {ok}/{len(files)} valid, {bad} invalid"
        + (f", {object_bytes / 1e6:.2f} MB object code (opt={emit_opt})"
           if emit_opt is not None else "")
        + (" written to " + str(emit_dir) if emit_dir is not None else "")
        + f" ({time.monotonic() - start:.1f}s; reused={len(reused)})"
    )
    if slowest[0]:
        print(f"slowest module: {slowest[0]:.2f}s {slowest[1]}")
    for key, count in errors.most_common(8):
        print(f"  {count:5d}x {key}\n         e.g. {first[key]}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
