#!/usr/bin/env python3
"""Offline tests for pinned Windows wgpu-native SDK bootstrap."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import tempfile
import zipfile


def main():
    script = Path(__file__).resolve().parents[2] / "tools" / "ensure_wgpu_native.py"
    spec = importlib.util.spec_from_file_location("ensure_wgpu_native", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory() as temp:
        parent = Path(temp)
        zip_path = parent / "fake.zip"
        entries = {
            "pkg/include/webgpu/webgpu.h": b"header",
            "pkg/include/webgpu/wgpu.h": b"extras",
            "pkg/lib/wgpu_native.lib": b"import lib",
            "pkg/bin/wgpu_native.dll": b"dll",
        }

        def make_zip(data):
            with zipfile.ZipFile(zip_path, "w") as zf:
                for name, content in data.items():
                    zf.writestr(name, content)

        make_zip(entries)
        correct_hash = hashlib.sha256(zip_path.read_bytes()).hexdigest()
        module.SDK_SHA256 = correct_hash
        module.download_archive = lambda destination: shutil.copyfile(zip_path, destination)
        # Match the repository's default CMake build/_deps layout.
        destination = parent / "build" / "_deps" / "wgpu-native"
        module.ensure_sdk(destination)
        assert module.sdk_ready(destination)
        assert (destination / "include/webgpu/webgpu.h").read_bytes() == b"header"
        assert (destination / "lib/wgpu_native.lib").read_bytes() == b"import lib"

        # Rerun must be offline and must preserve the original install.
        module.download_archive = lambda unused: (_ for _ in ()).throw(AssertionError("re-downloaded"))
        module.ensure_sdk(destination)

        # Missing files in a non-empty install are rejected rather than clobbered.
        (destination / "include/webgpu/webgpu.h").unlink()
        try:
            module.ensure_sdk(destination)
            raise AssertionError("incomplete install accepted")
        except ValueError as e:
            assert "incomplete" in str(e)

        # A bad checksum must not create a seemingly valid install.
        module.download_archive = lambda new: shutil.copyfile(zip_path, new)
        module.SDK_SHA256 = "0" * 64
        bad = parent / "bad"
        try:
            module.ensure_sdk(bad)
            raise AssertionError("bad sha accepted")
        except ValueError as e:
            assert "SHA-256 mismatch" in str(e)
        assert not bad.exists()

        # Traversal attempts must be rejected even inside a correctly hashed ZIP.
        make_zip(dict(entries, **{"pkg/../../bad-file": b"evil"}))
        module.SDK_SHA256 = hashlib.sha256(zip_path.read_bytes()).hexdigest()
        unsafe = parent / "unsafe"
        try:
            module.ensure_sdk(unsafe)
            raise AssertionError("unsafe zip accepted")
        except ValueError as e:
            assert "unsafe filename" in str(e)
        assert not unsafe.exists()
    print("ensure_wgpu_native: PASS (install, cached, incomplete, hash, traversal)")


if __name__ == "__main__":
    main()
