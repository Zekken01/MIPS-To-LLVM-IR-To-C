#!/usr/bin/env python3
"""Download and cache the latest official OpenPS2BIOS ROM0 for SDBZ boot.

The current latest non-prerelease GitHub release is checked at each launch.
Only the upstream 4 MiB .bin asset is accepted, verified against its
published .sha256 companion (or GitHub's sha256 asset digest). No private
or Sony BIOS files are downloaded. Existing verified cache is usable offline.
An explicit SDBZ_IOP_ROM0 environment override is handled by the caller.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request


API_LATEST = (
    "https://api.github.com/repos/TamaribaEmu/OpenPS2BIOS/releases/latest"
)
ROM0_SIZE = 4 * 1024 * 1024
ROM0_NAME = "rom0.bin"
META_NAME = "release.json"
USER_AGENT = "MIPS-To-LLVM-IR-To-C-OpenPS2BIOS-bootstrap"
VERSION_PATTERN = re.compile(r"^v?(\d+(?:\.\d+)*)$")
SHA_PATTERN = re.compile(r"^[a-fA-F0-9]{64}$")


def open_github(url):
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "application/vnd.github+json",
        },
    )
    return urllib.request.urlopen(request, timeout=30)


def fetch_latest_release():
    with open_github(API_LATEST) as response:
        payload = response.read(1024 * 1024 + 1)
    if len(payload) > 1024 * 1024:
        raise ValueError("latest-release metadata exceeded size limit")
    data = json.loads(payload)
    if not isinstance(data, dict) or data.get("prerelease") or data.get("draft"):
        raise ValueError("latest release is invalid or not stable")
    return data


def verified_asset_url(asset):
    if not isinstance(asset, dict):
        raise ValueError("release asset metadata is invalid")
    url = asset.get("browser_download_url")
    if not isinstance(url, str):
        raise ValueError("release asset has no download URL")
    parsed = urllib.parse.urlsplit(url)
    if (
        parsed.scheme != "https"
        or parsed.hostname != "github.com"
        or not parsed.path.startswith(
            "/TamaribaEmu/OpenPS2BIOS/releases/download/"
        )
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError("release asset URL is not an official GitHub download")
    return url


def select_release_assets(release):
    match = VERSION_PATTERN.fullmatch(str(release.get("tag_name", "")))
    if match is None:
        raise ValueError("latest OpenPS2BIOS release has an unknown tag")
    version = match.group(1)
    filename = f"OpenPS2BIOS-{version}.bin"
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ValueError("latest release has no asset list")
    named = {item.get("name"): item for item in assets
             if isinstance(item, dict)}
    binary = named.get(filename)
    checksum = named.get(filename + ".sha256")
    if binary is None:
        raise ValueError(f"latest release has no {filename}")
    if binary.get("size") != ROM0_SIZE:
        raise ValueError("latest release ROM0 asset is not exactly 4 MiB")
    verified_asset_url(binary)
    if checksum is None:
        digest = binary.get("digest", "")
        if not isinstance(digest, str) or not digest.startswith("sha256:"):
            raise ValueError("release has neither SHA256 file nor asset digest")
        if not SHA_PATTERN.fullmatch(digest[7:]):
            raise ValueError("GitHub asset SHA256 is invalid")
    else:
        verified_asset_url(checksum)
    return filename, binary, checksum


def download_bytes(url, max_size):
    with open_github(url) as response:
        payload = response.read(max_size + 1)
    if len(payload) > max_size:
        raise ValueError("upstream download exceeded size limit")
    return payload


def parse_sha256(contents, filename):
    if len(contents) > 4096:
        raise ValueError("SHA256 companion exceeded size limit")
    for raw_line in contents.decode("utf-8-sig").splitlines():
        fields = raw_line.strip().split()
        if not fields:
            continue
        if SHA_PATTERN.fullmatch(fields[0]):
            if len(fields) == 1 or (
                len(fields) == 2 and fields[1].lstrip("*") == filename
            ):
                return fields[0].lower()
    raise ValueError("SHA256 companion does not identify the ROM0 asset")


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validated_cache(cache_dir):
    image = cache_dir / ROM0_NAME
    metadata = cache_dir / META_NAME
    try:
        info = json.loads(metadata.read_text(encoding="utf-8"))
        expected = info.get("sha256")
        if (
            isinstance(expected, str)
            and SHA_PATTERN.fullmatch(expected)
            and image.stat().st_size == ROM0_SIZE
            and sha256_file(image) == expected.lower()
        ):
            return info
    except (OSError, ValueError, AttributeError, TypeError):
        pass
    return None


def write_atomically(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(contents)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def ensure_rom0(cache_dir):
    cache_dir = Path(cache_dir).resolve()
    cache = validated_cache(cache_dir)
    try:
        release = fetch_latest_release()
    except (OSError, ValueError, urllib.error.URLError,
            json.JSONDecodeError) as exc:
        if cache is not None:
            print(
                f"OpenPS2BIOS latest release check failed ({exc}); "
                f"using verified cached {cache.get('tag_name')}",
                file=sys.stderr,
            )
            return cache_dir / ROM0_NAME
        raise RuntimeError(
            "cannot check the latest OpenPS2BIOS release and no "
            "verified cached ROM0 exists"
        ) from exc

    filename, binary, checksum = select_release_assets(release)
    release_id = release.get("id")
    asset_id = binary.get("id")
    if (
        cache is not None
        and cache.get("release_id") == release_id
        and cache.get("asset_id") == asset_id
    ):
        print(
            f"OpenPS2BIOS {release['tag_name']}: verified cached ROM0",
            file=sys.stderr,
        )
        return cache_dir / ROM0_NAME

    print(
        f"OpenPS2BIOS {release['tag_name']}: downloading {filename}",
        file=sys.stderr,
    )
    if checksum is not None:
        checksum_data = download_bytes(verified_asset_url(checksum), 4096)
        expected_sha = parse_sha256(checksum_data, filename)
    else:
        expected_sha = binary["digest"][7:].lower()

    try:
        image = download_bytes(verified_asset_url(binary), ROM0_SIZE)
        if len(image) != ROM0_SIZE:
            raise ValueError("downloaded ROM0 is not exactly 4 MiB")
        actual_sha = hashlib.sha256(image).hexdigest()
        if actual_sha != expected_sha:
            raise ValueError(
                "official ROM0 SHA256 mismatch: "
                f"expected {expected_sha}, got {actual_sha}"
            )
    except (OSError, ValueError, urllib.error.URLError) as exc:
        if cache is not None:
            print(
                f"OpenPS2BIOS update failed ({exc}); "
                f"using verified cached {cache.get('tag_name')}",
                file=sys.stderr,
            )
            return cache_dir / ROM0_NAME
        raise

    # Write the validated image before metadata. A crash between these
    # operations leaves an invalid cache entry, never an unverified hit.
    cache_dir.mkdir(parents=True, exist_ok=True)
    write_atomically(cache_dir / ROM0_NAME, image)
    write_atomically(
        cache_dir / META_NAME,
        (json.dumps({
            "schema": "openps2bios-release-cache-v1",
            "tag_name": release["tag_name"],
            "release_id": release_id,
            "asset_id": asset_id,
            "asset_name": filename,
            "sha256": expected_sha,
        }, indent=2) + "\n").encode("utf-8"),
    )
    print(
        f"OpenPS2BIOS {release['tag_name']}: verified "
        f"4 MiB ROM0 sha256={expected_sha}",
        file=sys.stderr,
    )
    return cache_dir / ROM0_NAME


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=Path("build/openps2bios"))
    args = parser.parse_args()
    try:
        path = ensure_rom0(args.cache_dir)
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as exc:
        print(f"OpenPS2BIOS bootstrap failed: {exc}", file=sys.stderr)
        return 1
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
