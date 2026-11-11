#!/usr/bin/env python3
"""Parse/verify emitted LLVM IR and optionally emit host objects with llvmlite.

Examples:
  python3 tools/verify_ir.py 'out/*.ll'
  python3 tools/verify_ir.py 'out/*.ll' --emit
  python3 tools/verify_ir.py 'out/*.ll' --emit-opt=0 --progress=250
  python3 tools/verify_ir.py 'out/*.ll' --emit-dir=out/objects --emit-opt=0

`--emit` defaults to the LLVM target-machine optimization level 2.
`--emit-opt=0` is a faster backend pass over large game outputs.
Without `--emit-dir`, emitted object bytes are counted in memory only.
With `--emit-dir`, one relocatable object is written per input module and an
objects.json manifest records its symbol, size and SHA-256 digest.

Native code generation is explicitly retargeted to the current host triple and
data layout. The guest IR's original Windows/MSVC data layout must not be used
to emit a host object on a different platform.
"""
import collections
import glob
import hashlib
import json
import re
import sys
import time
from pathlib import Path

import llvmlite.binding as llvm


def write_atomic(path, data):
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(data)
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main():
    args = sys.argv[1:]
    if not args or "-h" in args or "--help" in args:
        print(__doc__)
        return 0 if args else 2

    emit_opt = None
    emit_dir = None
    progress_every = 250
    patterns = []
    for arg in args:
        if arg == "--emit":
            emit_opt = 2
        elif arg.startswith("--emit-opt="):
            emit_opt = int(arg.split("=", 1)[1])
            if emit_opt < 0 or emit_opt > 3:
                raise SystemExit("--emit-opt must be between 0 and 3")
        elif arg.startswith("--emit-dir="):
            emit_dir = Path(arg.split("=", 1)[1])
            if not str(emit_dir):
                raise SystemExit("--emit-dir requires a directory")
            if emit_opt is None:
                emit_opt = 0
        elif arg.startswith("--progress="):
            progress_every = max(1, int(arg.split("=", 1)[1]))
        else:
            patterns.append(arg)

    files = sorted({str(Path(name)) for pattern in patterns for name in glob.glob(pattern)})
    if not files:
        print("No LLVM IR files matched the supplied pattern(s).")
        return 2

    tm = None
    target_triple = None
    if emit_opt is not None:
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        target_triple = llvm.get_default_triple()
        target = llvm.Target.from_triple(target_triple)
        tm = target.create_target_machine(
            opt=emit_opt, reloc="pic", codemodel="default"
        )

    if emit_dir is not None:
        emit_dir.mkdir(parents=True, exist_ok=True)
        object_names = [Path(filename).stem + ".o" for filename in files]
        if len(set(object_names)) != len(object_names):
            raise SystemExit("input modules have duplicate basenames; refusing to overwrite objects")

    ok = bad = 0
    object_bytes = 0
    errors = collections.Counter()
    first = {}
    emitted = []
    start = time.time()
    slowest = (0.0, "")

    for index, filename in enumerate(files, 1):
        module_start = time.time()
        try:
            module = llvm.parse_assembly(Path(filename).read_text(encoding="utf-8"))
            module.verify()

            if tm is not None:
                # The emitted IR describes the EE state, but its checked-in
                # target triple is Windows/MSVC. Object files must match the
                # host that will load them, not that original IR target.
                module.triple = target_triple
                module.data_layout = str(tm.target_data)
                module.verify()

                object_data = tm.emit_object(module)
                object_bytes += len(object_data)

                if emit_dir is not None:
                    object_name = Path(filename).stem + ".o"
                    object_path = emit_dir / object_name
                    write_atomic(object_path, object_data)
                    symbols = [
                        function.name for function in module.functions
                        if not function.is_declaration
                    ]
                    emitted.append({
                        "input": str(Path(filename)),
                        "object": object_name,
                        "size_bytes": len(object_data),
                        "sha256": hashlib.sha256(object_data).hexdigest(),
                        "defined_symbols": symbols,
                    })
            ok += 1
        except Exception as exc:
            bad += 1
            lines = str(exc).strip().splitlines()
            error_lines = [line for line in lines if "error:" in line]
            key = re.sub(
                r"\d+",
                "N",
                re.sub(r"%\w+", "%X", (error_lines[0] if error_lines else lines[0])),
            )[:110]
            errors[key] += 1
            first.setdefault(key, filename)

        elapsed = time.time() - module_start
        if elapsed > slowest[0]:
            slowest = (elapsed, filename)
        if index % progress_every == 0 or index == len(files):
            mode = f", object bytes {object_bytes:,}" if tm is not None else ""
            print(
                f"[{index}/{len(files)}] valid={ok} invalid={bad}{mode} "
                f"elapsed={time.time() - start:.1f}s",
                flush=True,
            )

    if emit_dir is not None:
        manifest = {
            "complete": bad == 0 and len(emitted) == len(files),
            "llvm_version": ".".join(map(str, llvm.llvm_version_info)),
            "target_triple": target_triple,
            "optimization_level": emit_opt,
            "input_module_count": len(files),
            "object_count": len(emitted),
            "object_bytes": sum(item["size_bytes"] for item in emitted),
            "objects": emitted,
        }
        write_atomic(
            emit_dir / "objects.json",
            (json.dumps(manifest, indent=2) + "\n").encode("utf-8"),
        )

    print(
        f"LLVM {llvm.llvm_version_info}: {ok}/{len(files)} valid, {bad} invalid"
        + (f", {object_bytes / 1e6:.2f} MB object code (opt={emit_opt})" if tm is not None else "")
        + (" written to " + str(emit_dir) if emit_dir is not None else "")
        + f" ({time.time() - start:.1f}s)"
    )
    if slowest[0]:
        print(f"slowest module: {slowest[0]:.2f}s {slowest[1]}")
    for key, count in errors.most_common(8):
        print(f"  {count:5d}x {key}\n         e.g. {first[key]}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
