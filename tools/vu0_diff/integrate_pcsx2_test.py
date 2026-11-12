#!/usr/bin/env python3
"""Install/remove the pinned PCSX2 VU0 issue-order and trace tests."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


PCSX2_REVISION = "82cac38b2af19d593dedfd9ddaa63448e7ec493c"
TEST_RELATIVE_PATH = Path("tests/ctest/core/vu0_issue_order_test.cpp")
FIXTURE_RELATIVE_PATH = Path("tests/ctest/core/vu0_trace_fixture.h")
CMAKE_RELATIVE_PATH = Path("tests/ctest/core/CMakeLists.txt")
CMAKE_ANCHOR = "\tStubHost.cpp\n)"
CMAKE_INSERTION = "\tStubHost.cpp\n\tvu0_issue_order_test.cpp\n)"
CMAKE_MARKER = "\tvu0_issue_order_test.cpp"


def get_revision(source_root: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def install(source_root: Path, adapter_source: Path, fixture_source: Path) -> None:
    revision = get_revision(source_root)
    if revision != PCSX2_REVISION:
        raise RuntimeError(
            f"PCSX2 revision mismatch: expected {PCSX2_REVISION}, got {revision}"
        )

    cmake_path = source_root / CMAKE_RELATIVE_PATH
    test_path = source_root / TEST_RELATIVE_PATH
    fixture_path = source_root / FIXTURE_RELATIVE_PATH
    cmake_text = cmake_path.read_text(encoding="utf-8")

    if CMAKE_MARKER in cmake_text:
        if not test_path.is_file() or not fixture_path.is_file():
            raise RuntimeError(
                "PCSX2 CMakeLists references the VU0 adapter, "
                "but the adapter source or shared fixture is missing"
            )
        print("PCSX2 VU0 issue-order and trace tests are already installed.")
        return

    if CMAKE_ANCHOR not in cmake_text:
        raise RuntimeError(
            f"Could not find the expected CMake anchor in {cmake_path}"
        )
    if test_path.exists() or fixture_path.exists():
        raise RuntimeError(
            "Refusing to overwrite an existing PCSX2 test source or fixture"
        )

    test_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        test_path.write_bytes(adapter_source.read_bytes())
        fixture_path.write_bytes(fixture_source.read_bytes())
        cmake_path.write_text(
            cmake_text.replace(CMAKE_ANCHOR, CMAKE_INSERTION, 1),
            encoding="utf-8",
            newline="",
        )
    except OSError:
        if test_path.exists():
            test_path.unlink()
        if fixture_path.exists():
            fixture_path.unlink()
        raise

    print(
        f"Installed the VU0 issue-order and trace tests into PCSX2 {revision}."
    )


def uninstall(source_root: Path) -> None:
    revision = get_revision(source_root)
    if revision != PCSX2_REVISION:
        raise RuntimeError(
            f"PCSX2 revision mismatch: expected {PCSX2_REVISION}, got {revision}"
        )

    cmake_path = source_root / CMAKE_RELATIVE_PATH
    test_path = source_root / TEST_RELATIVE_PATH
    fixture_path = source_root / FIXTURE_RELATIVE_PATH
    cmake_text = cmake_path.read_text(encoding="utf-8")

    if CMAKE_MARKER in cmake_text:
        if CMAKE_INSERTION not in cmake_text:
            raise RuntimeError(
                "CMake marker exists but expected source-list text does not; "
                "refusing to edit an unknown CMake layout"
            )
        cmake_text = cmake_text.replace(CMAKE_INSERTION, CMAKE_ANCHOR, 1)
        cmake_path.write_text(cmake_text, encoding="utf-8", newline="")

    if test_path.exists():
        test_path.unlink()
    if fixture_path.exists():
        fixture_path.unlink()

    print("Removed the PCSX2 VU0 issue-order and trace tests.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pcsx2-root",
        type=Path,
        required=True,
        help="Path to a clean PCSX2 checkout at the pinned revision",
    )
    parser.add_argument(
        "--remove",
        action="store_true",
        help="Remove the tests from the PCSX2 checkout",
    )
    args = parser.parse_args()

    source_root = args.pcsx2_root.resolve()
    if not (source_root / ".git").exists():
        print("error: --pcsx2-root is not a Git checkout", file=sys.stderr)
        return 2

    adapter_source = Path(__file__).with_name("pcsx2_issue_order_test.cpp")
    fixture_source = Path(__file__).with_name("vu0_trace_fixture.h")
    try:
        if args.remove:
            uninstall(source_root)
        else:
            install(source_root, adapter_source, fixture_source)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
