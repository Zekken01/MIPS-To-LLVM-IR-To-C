#!/usr/bin/env python3
"""End-to-end smoke test for writing a relocatable object from an IR module."""

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_object_emission.py compare_fixture verify_ir_script"
        )

    fixture = pathlib.Path(sys.argv[1]).resolve()
    verify_script = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="ee-object-emission-") as temp:
        root = pathlib.Path(temp)
        ir_path = root / "scalar_compare.ll"
        object_dir = root / "objects"

        subprocess.run([str(fixture), str(ir_path)], check=True)
        subprocess.run(
            [
                sys.executable,
                str(verify_script),
                str(ir_path),
                "--emit-dir=" + str(object_dir),
                "--progress=1",
            ],
            check=True,
        )

        object_path = object_dir / "scalar_compare.o"
        manifest_path = object_dir / "objects.json"
        require(object_path.is_file(), "native object file was not written")
        require(manifest_path.is_file(), "object manifest was not written")

        object_data = object_path.read_bytes()
        require(len(object_data) > 256, "native object is unexpectedly small")
        require(object_data[:4] == b"\x7fELF",
                "host object is not an ELF relocatable object")

        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        require(manifest["complete"], "object manifest reports incomplete emission")
        require(manifest["input_module_count"] == 1, "wrong input module count")
        require(manifest["object_count"] == 1, "wrong emitted object count")
        require(manifest["target_triple"] == manifest["target_triple"].strip(),
                "target triple is empty or malformed")

        entry = manifest["objects"][0]
        require(entry["object"] == object_path.name, "manifest object path mismatch")
        require(entry["size_bytes"] == len(object_data), "manifest object size mismatch")
        require(
            entry["sha256"] == hashlib.sha256(object_data).hexdigest(),
            "manifest object digest mismatch",
        )
        require(
            "guest_scalar_compare_test" in entry["defined_symbols"],
            "manifest is missing the generated guest function symbol",
        )

    print("object_emission: PASS")


if __name__ == "__main__":
    main()
