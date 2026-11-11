#!/usr/bin/env python3
"""Parse/verify emitted LLVM IR and optionally emit host objects with llvmlite.

Examples:
  python3 tools/verify_ir.py 'out/*.ll'
  python3 tools/verify_ir.py 'out/*.ll' --emit
  python3 tools/verify_ir.py 'out/*.ll' --emit-opt=0 --progress=250

`--emit` defaults to the LLVM target-machine optimization level 2. Use
`--emit-opt=0` for a faster backend smoke pass over very large game outputs.
A zero exit status means every selected module parsed and passed LLVM verify.
"""
import collections
import glob
import re
import sys
import time
from pathlib import Path

import llvmlite.binding as llvm


def main():
    args = sys.argv[1:]
    if not args or "-h" in args or "--help" in args:
        print(__doc__)
        return 0 if args else 2

    emit_opt = None
    progress_every = 250
    patterns = []
    for arg in args:
        if arg == "--emit":
            emit_opt = 2
        elif arg.startswith("--emit-opt="):
            emit_opt = int(arg.split("=", 1)[1])
            if emit_opt < 0 or emit_opt > 3:
                raise SystemExit("--emit-opt must be between 0 and 3")
        elif arg.startswith("--progress="):
            progress_every = max(1, int(arg.split("=", 1)[1]))
        else:
            patterns.append(arg)

    files = sorted({name for pattern in patterns for name in glob.glob(pattern)})
    if not files:
        print("No LLVM IR files matched the supplied pattern(s).")
        return 2

    tm = None
    if emit_opt is not None:
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        target = llvm.Target.from_default_triple()
        tm = target.create_target_machine(
            opt=emit_opt, reloc="pic", codemodel="default"
        )

    ok = bad = 0
    object_bytes = 0
    errors = collections.Counter()
    first = {}
    start = time.time()
    slowest = (0.0, "")

    for index, filename in enumerate(files, 1):
        module_start = time.time()
        try:
            module = llvm.parse_assembly(Path(filename).read_text())
            module.verify()
            if tm is not None:
                object_bytes += len(tm.emit_object(module))
            ok += 1
        except Exception as exc:
            bad += 1
            lines = str(exc).strip().splitlines()
            error_lines = [line for line in lines if "error:" in line]
            key = re.sub(r"\d+", "N", re.sub(r"%\w+", "%X", (error_lines[0] if error_lines else lines[0])))[:110]
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

    print(
        f"LLVM {llvm.llvm_version_info}: {ok}/{len(files)} valid, {bad} invalid"
        + (f", {object_bytes / 1e6:.2f} MB object code (opt={emit_opt})" if tm is not None else "")
        + f" ({time.time() - start:.1f}s)"
    )
    if slowest[0]:
        print(f"slowest module: {slowest[0]:.2f}s {slowest[1]}")
    for key, count in errors.most_common(8):
        print(f"  {count:5d}x {key}\n         e.g. {first[key]}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
