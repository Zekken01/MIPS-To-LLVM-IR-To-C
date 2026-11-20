#!/usr/bin/env python3
"""Run a deterministic CTest shard from an already-configured build.

The seven shards are mutually exclusive and exhaustive: four independent
native game-runner integration groups run in parallel jobs, and every other
test is distributed across three short-test jobs. CTest's JSON listing is the source
of truth, so newly registered tests cannot silently fall between number ranges.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

GAME_TESTS = tuple(f"game_runner_end_to_end_{i}" for i in range(4))
SHORT_SHARDS = 3
SHARDS = tuple(f"game-{i}" for i in range(4)) + (
    "fast-0", "fast-1", "fast-2",
)


def select_tests(tests: list[str], shard: str) -> list[str]:
    """Partition the CTest listing with no overlap and no omissions."""
    if len(set(tests)) != len(tests):
        raise ValueError("CTest returned duplicate test names")

    if shard.startswith("game-"):
        name = GAME_TESTS[int(shard.rsplit("-", 1)[1])]
        return [item for item in tests if item == name]

    fast_tests = sorted(name for name in tests if name not in GAME_TESTS)
    index = int(shard.rsplit("-", 1)[1])
    return [
        name
        for position, name in enumerate(fast_tests)
        if position % SHORT_SHARDS == index
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--shard", required=True, choices=SHARDS)
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    metadata = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    tests = [entry["name"] for entry in json.loads(metadata.stdout)["tests"]]

    # The build job configures llvmlite and must register the real-game test.
    # Otherwise, losing the long-running shard would go unnoticed.
    missing_game = set(GAME_TESTS) - set(tests)
    if missing_game:
        print(f"Missing mandatory CTest tests: {sorted(missing_game)}",
              file=sys.stderr)
        return 2

    selected = select_tests(tests, args.shard)
    if not selected:
        print(f"Empty CTest shard: {args.shard}", file=sys.stderr)
        return 2

    # Check the partition every time, so adding or renaming tests cannot
    # silently drop or double-run a CTest entry in CI.
    coverage = [name for shard in SHARDS for name in select_tests(tests, shard)]
    if sorted(coverage) != sorted(tests):
        print("CTest shard coverage differs from configured tests", file=sys.stderr)
        return 2

    print(
        f"CTest shard {args.shard}: {len(selected)}/{len(tests)} tests",
        flush=True,
    )
    print("\n".join(f"  {name}" for name in selected), flush=True)
    exact_names = "^(" + "|".join(re.escape(name) for name in selected) + ")$"
    report = build_dir / "Testing" / f"ctest-{args.shard}.xml"
    report.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [
            "ctest", "--test-dir", str(build_dir), "--output-on-failure",
            "--tests-regex", exact_names,
            "--output-junit", str(report),
        ],
        check=False,
    )
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
