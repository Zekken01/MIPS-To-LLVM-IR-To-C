#!/usr/bin/env python3
"""Regression checks for the fail-closed offline EE timing comparator."""

import csv
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
import ee_timing_differential as differential  # noqa: E402


def save(path, columns, events):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(columns)
        writer.writerows(events)


def expect_failure(candidate, reference):
    try:
        differential.compare(candidate, reference)
    except ValueError:
        return
    raise AssertionError("invalid/incompatible reference trace was accepted")


def main() -> int:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        trial = root / "candidate.csv"
        reference = root / "reference.csv"
        rows = [
            (1, "0x00001000", "0x24080001", 0, 0),
            (2, "0x00001004", "0x24090002", 2, 0),
            (3, "0x00001008", "0x51600001", 0, 1),
            (4, "0x0000100c", "0x240a0003", 1, 2),
        ]
        save(trial, differential.CANDIDATE_COLUMNS, rows)
        save(reference, differential.REFERENCE_COLUMNS,
             [(*row[:4], row[-1] + 12345) for row in rows])
        assert differential.compare(trial, reference) == 0

        bad = [(*row[:4], row[-1] + 12345) for row in rows]
        bad[2] = (*bad[2][:4], bad[2][-1] + 1)
        save(reference, differential.REFERENCE_COLUMNS, bad)
        expect_failure(trial, reference)

        bad = [(*row[:4], row[-1] + 12345) for row in rows]
        bad[1] = (bad[1][0], bad[1][1], "0xffffffff", bad[1][3], bad[1][4])
        save(reference, differential.REFERENCE_COLUMNS, bad)
        expect_failure(trial, reference)

        save(reference, differential.REFERENCE_COLUMNS,
             [(*row[:4], row[-1] + 12345) for row in rows[:-1]])
        expect_failure(trial, reference)

        bad = [(*row[:4], row[-1] + 12345) for row in rows]
        bad[2] = (2, *bad[2][1:])
        save(reference, differential.REFERENCE_COLUMNS, bad)
        expect_failure(trial, reference)

        bad = [(*row[:4], row[-1] + 12345) for row in rows]
        bad[1] = (bad[1][0], bad[1][1], bad[1][2], 3, bad[1][4])
        save(reference, differential.REFERENCE_COLUMNS, bad)
        expect_failure(trial, reference)

    print("test_ee_timing_differential: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
