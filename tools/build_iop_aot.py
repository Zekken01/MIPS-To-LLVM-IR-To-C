#!/usr/bin/env python3
"""Build the ahead-of-time IOP IRX library from a runner capture.

1. Capture: run the game once with SDBZ_IOP_AOT_CAPTURE=CAPTURE_DIR. Every
   loaded IRX writes its relocated, linked .text and metadata.
2. This script: iop_recomp (C++) translates each module to LLVM IR, clang
   compiles the IR objects in parallel, and lld links iop-translated.dll/.so.
3. Run the game with SDBZ_IOP_AOT=<library>. Modules whose bytes in IOP RAM
   match the capture run translated code; everything else is interpreted.

usage: build_iop_aot.py --build-dir build --capture DIR --out DIR [--opt 2]
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", default="2", choices=["0", "1", "2", "3"])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    exe = ".exe" if os.name == "nt" else ""
    recomp = args.build_dir / f"iop_recomp{exe}"
    if not recomp.is_file():
        recomp = args.build_dir / "Release" / f"iop_recomp{exe}"
    clang = shutil.which("clang")
    if clang is None:
        sys.exit("build_iop_aot: clang not found on PATH")

    ir_dir = args.out / "ir"
    obj_dir = args.out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    for stale in list(ir_dir.glob("*.ll")) + list(obj_dir.glob("*.o")):
        stale.unlink()
    started = time.monotonic()
    subprocess.run([str(recomp), str(args.capture), str(ir_dir)], check=True)
    sources = sorted(ir_dir.glob("*.ll"))

    def compile_one(source):
        obj = obj_dir / (source.stem + ".o")
        subprocess.run([clang, f"-O{args.opt}", "-c", "-Wno-override-module",
                        str(source), "-o", str(obj)], check=True)
        return obj

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        objects = list(pool.map(compile_one, sources))
    compiled = time.monotonic()

    library = args.out / ("iop-translated.dll" if os.name == "nt"
                          else "iop-translated.so")
    command = [clang, "-shared", "-fuse-ld=lld", "-o", str(library)]
    command += [str(obj) for obj in objects]
    if os.name == "nt":
        command += ["-nostdlib", "-Wl,/noentry"]
    subprocess.run(command, check=True)
    print(f"build_iop_aot: {len(sources) - 1} module(s) -> {library} "
          f"(compile {compiled - started:.1f}s, total "
          f"{time.monotonic() - started:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
