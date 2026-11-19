#!/usr/bin/env python3
"""Install the pinned native Windows WebGPU SDK required by EE_ENABLE_WEBGPU.

Only standard-library modules are used. The ZIP SHA-256 is pinned to the
Windows-MSVC package used in the repository's native WebGPU CI workflow.
"""

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tempfile
import urllib.request
import zipfile

SDK_URL = (
    "https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/"
    "wgpu-windows-x86_64-msvc-release.zip"
)
SDK_SHA256 = "7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132"
REQUIRED_FILES = (
    "include/webgpu/webgpu.h",
    "include/webgpu/wgpu.h",
    "lib/wgpu_native.lib",
)
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_EXPANDED_BYTES = 1024 * 1024 * 1024


def sdk_ready(root: Path) -> bool:
    return all((root / name).is_file() for name in REQUIRED_FILES) and (
        (root / "lib/wgpu_native.dll").is_file() or
        (root / "bin/wgpu_native.dll").is_file()
    )


def download_archive(path: Path) -> None:
    request = urllib.request.Request(SDK_URL, headers={"User-Agent": "ps2-native-webgpu/1.0"})
    with urllib.request.urlopen(request, timeout=90) as response, path.open("wb") as output:
        total = 0
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_ARCHIVE_BYTES:
                raise ValueError("SDK archive exceeds the permitted download size")
            output.write(chunk)


def verify_archive(path: Path) -> None:
    sha = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            sha.update(chunk)
    if sha.hexdigest().lower() != SDK_SHA256:
        raise ValueError("wgpu-native SDK SHA-256 mismatch; refusing unverified archive")


def unpack_archive(archive: Path, output: Path) -> None:
    with zipfile.ZipFile(archive) as zf:
        files = []
        cumulative_size = 0
        for entry in zf.infolist():
            filename = entry.filename
            path = PurePosixPath(filename)
            if (not filename or filename.startswith("/") or "\\" in filename or
                ":" in filename or any(part in ("", ".", "..") for part in filename.split("/") if part) or
                ".." in path.parts or
                (entry.create_system == 3 and
                 stat.S_IFMT(entry.external_attr >> 16) == stat.S_IFLNK)):
                raise ValueError("unsafe filename in wgpu-native SDK archive")
            if entry.is_dir():
                continue
            cumulative_size += entry.file_size
            if cumulative_size > MAX_EXPANDED_BYTES:
                raise ValueError("SDK archive expands beyond permitted size")
            files.append(entry)

        header = REQUIRED_FILES[0]
        prefixes = [entry.filename[:-len(header)] for entry in files
                    if entry.filename.endswith(header)]
        if len(prefixes) != 1:
            raise ValueError("SDK must contain exactly one include/webgpu/webgpu.h")
        prefix = prefixes[0]
        names = {entry.filename for entry in files}
        for filename in REQUIRED_FILES:
            if prefix + filename not in names:
                raise ValueError(f"SDK ZIP is missing {filename}")
        if not ({prefix + "lib/wgpu_native.dll", prefix + "bin/wgpu_native.dll"} & names):
            raise ValueError("SDK ZIP is missing wgpu_native.dll")

        seen = set()
        for entry in files:
            if not entry.filename.startswith(prefix):
                # Ignore archive-level metadata outside the SDK folder.
                continue
            relative = entry.filename[len(prefix):]
            if not relative:
                continue
            dest = Path(relative)
            lowered = str(dest).casefold()
            if lowered in seen:
                raise ValueError("SDK ZIP contains colliding destination paths")
            seen.add(lowered)
            target = output / dest
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(entry) as reader, target.open("wb") as writer:
                shutil.copyfileobj(reader, writer, 1024 * 1024)

    if not sdk_ready(output):
        raise ValueError("extracted wgpu-native SDK has an incomplete layout")


def ensure_sdk(root: Path) -> None:
    root = root.resolve()
    if sdk_ready(root):
        print(f"[wgpu-native] verified installed SDK: {root}")
        return
    if root.exists() and (not root.is_dir() or any(root.iterdir())):
        raise ValueError(
            f"SDK directory exists but is incomplete: {root}. "
            "Repair or empty it before retrying; no user files were overwritten."
        )

    root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wgpu-native-", dir=str(root.parent)) as temp:
        temp_root = Path(temp)
        archive = temp_root / "wgpu-native.zip"
        staged = temp_root / "sdk"
        staged.mkdir()
        print("[wgpu-native] fetching pinned Windows x64 MSVC SDK (v29.0.1.1)...", flush=True)
        download_archive(archive)
        verify_archive(archive)
        unpack_archive(archive, staged)
        if root.exists():
            root.rmdir()  # Only an empty directory is allowed here.
        os.replace(staged, root)
    print(f"[wgpu-native] ready: {root}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="SDK installation root containing include/ and lib/")
    args = parser.parse_args()
    try:
        ensure_sdk(args.root)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"[wgpu-native] ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
