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


def test_append_only_emission():
    with tempfile.TemporaryDirectory(prefix="ee-ir-append-") as temp:
        root = Path(temp)
        ir_dir = root / "ir"
        obj_dir = root / "objects"
        ir_dir.mkdir()
        old = ir_dir / "old_guest.ll"
        new = ir_dir / "new_guest.ll"
        later = ir_dir / "later_guest.ll"
        write_ir(old, 1)
        out, original = run_emitter(ir_dir, obj_dir)
        original_object = obj_dir / "old_guest.o"
        old_sha = hashlib.sha256(original_object.read_bytes()).hexdigest()
        os.utime(original_object, ns=(1000000000000000000, 1000000000000000000))

        def run_append(path, opt=0):
            return subprocess.run([
                sys.executable, str(SCRIPT), str(path),
                f"--emit-dir={obj_dir}", f"--emit-opt={opt}",
                "--incremental", "--append-only"
            ], text=True, capture_output=True)

        write_ir(new, 2)
        result = run_append(new)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "append-only preserved 1 cached object(s) without rehash" in result.stdout
        current = json.loads((obj_dir / "objects.json").read_text())
        assert current["complete"] and current["object_count"] == 2
        assert current["input_module_count"] == 2
        assert original_object.stat().st_mtime_ns == 1000000000000000000
        assert hashlib.sha256(original_object.read_bytes()).hexdigest() == old_sha
        assert current["objects"][0]["sha256"] == old_sha

        manifest_bytes = (obj_dir / "objects.json").read_bytes()
        duplicate = run_append(old)
        assert duplicate.returncode != 0
        assert "cannot replace an existing object" in duplicate.stderr
        assert (obj_dir / "objects.json").read_bytes() == manifest_bytes

        write_ir(later, 3)
        incompatible = run_append(later, opt=1)
        assert incompatible.returncode != 0
        assert "stale or incompatible" in incompatible.stderr
        assert (obj_dir / "objects.json").read_bytes() == manifest_bytes

        later.write_text("invalid IR syntax", encoding="utf-8")
        invalid = run_append(later)
        assert invalid.returncode != 0
        assert (obj_dir / "objects.json").read_bytes() == manifest_bytes

        write_ir(later, 3)
        valid = run_append(later)
        assert valid.returncode == 0, valid.stdout + valid.stderr
        three = json.loads((obj_dir / "objects.json").read_text())
        assert three["complete"] and three["object_count"] == 3
        assert len({item["object"] for item in three["objects"]}) == 3
        print("PASS: append-only LLVM emission, manifest safety and no old rehash")


if __name__ == "__main__":
    test_incremental_parallel_emission()
    test_append_only_emission()
