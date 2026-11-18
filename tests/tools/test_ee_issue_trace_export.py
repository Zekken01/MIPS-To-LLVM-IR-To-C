#!/usr/bin/env python3
"""Exercise complete candidate-event export and strict missing-time fail-closed."""

import csv
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_ee_issue_trace_export.py binary fixture.csv")
    binary = Path(sys.argv[1])
    fixture = Path(sys.argv[2])

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "candidate.csv"
        result = subprocess.run(
            [str(binary), str(fixture), "1", "3",
             f"--candidate-events={output}"],
            text=True, capture_output=True, check=False
        )
        assert result.returncode == 0, result.stdout + result.stderr
        with output.open("r", encoding="utf-8", newline="") as stream:
            events = list(csv.DictReader(stream))
        assert len(events) == 7, events
        assert [int(row["candidate_issue_cycle"]) for row in events] == [
            0, 0, 1, 4, 5, 6, 7
        ], events
        assert [int(row["reference_slot"]) for row in events] == list(
            range(1, 8)
        ), events
        assert events[5]["kind"] == "1", events[5]

        # Optional COP1 control/compare and separate SQRT/RSQRT groups
        # must be accepted without changing the old 4-argument CLI.
        numeric = ["1", "3", "4", "37", "1", "4",
                   "2", "4", "8", "4", "2", "4"]
        for extension in ([], ["8", "14"]):
            extended_path = root / f"extended-{len(extension)}.csv"
            extended = subprocess.run(
                [str(binary), str(fixture), *numeric, *extension,
                 f"--candidate-events={extended_path}"],
                text=True, capture_output=True, check=False
            )
            assert extended.returncode == 0, (
                extended.stdout, extended.stderr
            )
            assert extended_path.exists(), extended_path

        for values in (
            numeric[:-1],  # incomplete FCR31 group
            numeric[:-2] + ["0", "4"],  # unconfigured FCR31 control
            numeric + ["8", "0"],  # unconfigured RSQRT.S
        ):
            bad = subprocess.run(
                [str(binary), str(fixture), *values],
                text=True, capture_output=True, check=False
            )
            assert bad.returncode == 2, (
                values, bad.stdout, bad.stderr
            )

        # The replay must refuse HLE transitions and a partial trace,
        # without producing a deceptively complete candidate CSV.
        original = fixture.read_text(encoding="utf-8")
        for suffix, damaged in (
            ("hle", original.replace(
                "# truncated=false",
                "7,0x00001018,0x0000000c,3,7,118\n# truncated=false",
            )),
            ("truncated", original.replace(
                "# truncated=false", "# truncated=true"
            )),
        ):
            input_path = root / f"{suffix}.csv"
            rejected = root / f"{suffix}-output.csv"
            input_path.write_text(damaged, encoding="utf-8")
            failure = subprocess.run(
                [str(binary), str(input_path), "1", "3",
                 f"--candidate-events={rejected}"],
                text=True, capture_output=True, check=False
            )
            assert failure.returncode == 3, (
                suffix, failure.stdout, failure.stderr
            )
            assert not rejected.exists(), suffix

    print("test_ee_issue_trace_export: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
