#!/usr/bin/env python3
"""Compare a complete experimental EE issue timeline with an independent reference.

This tool does not validate an R5900 timing model, install hardware timing, or
authorise VBlank. It only checks exact event identity and relative cycles of
two externally supplied streams.
"""

import argparse
import csv
from pathlib import Path
import sys


CANDIDATE_COLUMNS = (
    "reference_slot", "pc", "opcode_word", "kind", "candidate_issue_cycle"
)
REFERENCE_COLUMNS = (
    "reference_slot", "pc", "opcode_word", "kind", "reference_issue_cycle"
)


def read_events(path: Path, fields: tuple[str, ...]) -> list[tuple[int, ...]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != fields:
            raise ValueError(f"{path}: unsupported columns {reader.fieldnames!r}")
        events = []
        last_cycle = -1
        for line, row in enumerate(reader, start=2):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"{path}:{line}: invalid column count")
            try:
                slot = int(row["reference_slot"], 10)
                pc = int(row["pc"], 0)
                opcode = int(row["opcode_word"], 0)
                kind = int(row["kind"], 10)
                cycle = int(row[fields[-1]], 10)
            except ValueError as error:
                raise ValueError(f"{path}:{line}: invalid integer") from error

            if (slot != len(events) + 1 or pc < 0 or pc > 0xFFFFFFFF or
                    opcode < 0 or opcode > 0xFFFFFFFF or
                    kind not in (0, 1, 2) or
                    cycle < 0 or cycle < last_cycle):
                raise ValueError(
                    f"{path}:{line}: non-contiguous slots, unsupported kind, "
                    "out-of-range operand or non-monotonic issue time"
                )
            last_cycle = cycle
            events.append((slot, pc, opcode, kind, cycle))

    if not events:
        raise ValueError(f"{path}: cannot compare empty timelines")
    return events


def compare(candidate: Path, reference: Path) -> int:
    trial = read_events(candidate, CANDIDATE_COLUMNS)
    known = read_events(reference, REFERENCE_COLUMNS)
    if len(trial) != len(known):
        raise ValueError(
            f"event count mismatch: candidate={len(trial)} reference={len(known)}"
        )

    # The externally captured reference may have a nonzero hardware epoch.
    # Only elapsed cycles relative to the first observed event are compared.
    trial_origin = trial[0][-1]
    known_origin = known[0][-1]
    for index, (c, r) in enumerate(zip(trial, known), start=1):
        if c[:4] != r[:4]:
            raise ValueError(
                f"event {index}: instruction identity differs: "
                f"candidate={c[:4]} reference={r[:4]}"
            )
        trial_delta = c[-1] - trial_origin
        known_delta = r[-1] - known_origin
        if trial_delta != known_delta:
            raise ValueError(
                f"event {index} PC=0x{c[1]:08x}: issue delta mismatch "
                f"candidate={trial_delta} reference={known_delta}"
            )

    print(
        f"EXACT RELATIVE MATCH: events={len(trial)} "
        f"candidate_span={trial[-1][-1] - trial_origin} "
        f"reference_span={known[-1][-1] - known_origin}; "
        "diagnostic only, not a validated R5900 clock"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    args = parser.parse_args()
    try:
        return compare(args.candidate, args.reference)
    except (ValueError, OSError) as error:
        print(f"EE timing differential rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
