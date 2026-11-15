#!/usr/bin/env python3
"""Synthetic LLVM IR regression tests for incremental parallel object emission."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "verify_ir.py"


def run_emitter(ir_dir, object_dir, opt=0):
    command = [
        sys.executable, str(SCRIPT), str(ir_dir / "*.ll"),
        f"--emit-dir={object_dir}", f"--emit-opt={opt}",
        "--incremental", "--jobs=2", "--progress=1",
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(
            f"emit failed ({result.returncode})\n{result.stdout}\n{result.stderr}"
        )
    return result.stdout, json.loads((object_dir / "objects.json").read_text())


def write_ir(path, value):
    path.write_text(
        f"define i32 @{path.stem}() {{\nentry:\n  ret i32 {value}\n}}\n",
        encoding="utf-8",
    )


def test_incremental_parallel_emission():
    with tempfile.TemporaryDirectory(prefix="ee-ir-cache-") as temp:
        root = Path(temp)
        ir_dir = root / "ir"
        object_dir = root / "objects"
        ir_dir.mkdir()
        first = ir_dir / "guest_fn_0x00100000.ll"
        second = ir_dir / "guest_fn_0x00100010.ll"
        write_ir(first, 1)
        write_ir(second, 2)
        out, manifest = run_emitter(ir_dir, object_dir)
        assert "reused 0/2" in out, out
        assert manifest["complete"] and manifest["object_count"] == 2
        assert len({item["sha256"] for item in manifest["objects"]}) == 2

        first_object = object_dir / (first.stem + ".o")
        second_object = object_dir / (second.stem + ".o")
        first_digest = hashlib.sha256(first_object.read_bytes()).hexdigest()
        second_digest = hashlib.sha256(second_object.read_bytes()).hexdigest()
        # A sentinel mtime proves a cache hit did not rewrite an object.
        os.utime(first_object, ns=(1000000000000000000, 1000000000000000000))
        out, manifest = run_emitter(ir_dir, object_dir)
        assert "reused 2/2" in out, out
        assert first_object.stat().st_mtime_ns == 1000000000000000000
        assert manifest["complete"]

        write_ir(second, 3)
        out, manifest = run_emitter(ir_dir, object_dir)
        assert "reused 1/2" in out, out
        assert first_object.stat().st_mtime_ns == 1000000000000000000
        assert hashlib.sha256(first_object.read_bytes()).hexdigest() == first_digest
        assert hashlib.sha256(second_object.read_bytes()).hexdigest() != second_digest

        # A tampered native object is never trusted even when its IR is unchanged.
        first_object.write_bytes(b"corrupt")
        out, manifest = run_emitter(ir_dir, object_dir)
        assert "reused 1/2" in out, out
        assert hashlib.sha256(first_object.read_bytes()).hexdigest() == first_digest
        assert manifest["complete"]

        # Backend optimization is part of the cache key.
        out, manifest = run_emitter(ir_dir, object_dir, opt=1)
        assert "reused 0/2" in out, out
        assert manifest["complete"] and manifest["optimization_level"] == 1
        print("PASS: parallel emission, IR change, cache hits, corruption, opt invalidation")


if __name__ == "__main__":
    test_incremental_parallel_emission()
