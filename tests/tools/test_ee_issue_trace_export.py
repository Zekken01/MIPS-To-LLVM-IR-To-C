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

        # Strict throughput is a separate, explicit offline candidate mode.
        # It must preserve the old no-unit trace without silently
        # installing hardware cycle calibration.
        strict_option = "--throughput=2,37,2,1,7,7,13"
        strict_output = root / "throughput.csv"
        strict = subprocess.run(
            [str(binary), str(fixture), "1", "3", strict_option,
             f"--candidate-events={strict_output}"],
            text=True, capture_output=True, check=False
        )
        assert strict.returncode == 0, strict.stdout + strict.stderr
        assert "throughput_mode=explicit" in strict.stdout, strict.stdout
        assert strict_output.read_text(encoding="utf-8") == (
            output.read_text(encoding="utf-8")
        )

        for invalid in (
            "--throughput=2,37,2,1,7,7",
            "--throughput=2,37,2,1,7,7,0",
            "--throughput=2,37,2,1,7,7,-1",
            "--throughput=2,37,2,1,7,7,13,9",
        ):
            rejected = subprocess.run(
                [str(binary), str(fixture), "1", "3", invalid],
                text=True, capture_output=True, check=False
            )
            assert rejected.returncode == 2, (
                invalid, rejected.stdout, rejected.stderr
            )

        mac_fixture = root / "mac.csv"
        mac_fixture.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            "1,0x00001000,0x00853018,0,1,0\n"
            "# truncated=false range=0x00001000:0x00001000\n",
            encoding="utf-8"
        )
        for argv_values in (
            ["1", "3", "7", "11", "4", "8"],
            ["1", "3", "7", "11", "4", "8", strict_option],
        ):
            result = subprocess.run(
                [str(binary), str(mac_fixture), *argv_values],
                text=True, capture_output=True, check=False
            )
            assert result.returncode == 0, (
                argv_values, result.stdout, result.stderr
            )

        # Repeated writes are the distinguishing fixture: conservative WAW
        # waiting sees 0,7, while explicit ordered overlap sees 0,2.
        two_mac = root / "two_mac.csv"
        two_mac.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            "1,0x00001000,0x00853018,0,1,0\n"
            "2,0x00001004,0x00853018,0,2,0\n"
            "# truncated=false range=0x00001000:0x00001004\n",
            encoding="utf-8"
        )
        for overlap, expected in (
            ([], [0, 7]),
            (["--ordered-waw-overlap"], [0, 2]),
        ):
            path = root / f"mac-overlap-{len(overlap)}.csv"
            candidate = subprocess.run(
                [str(binary), str(two_mac), "1", "3", "7", "11", "4",
                 "8", strict_option, *overlap, f"--candidate-events={path}"],
                text=True, capture_output=True, check=False
            )
            assert candidate.returncode == 0, (
                candidate.stdout, candidate.stderr
            )
            with path.open("r", encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            assert [int(x["candidate_issue_cycle"]) for x in rows] == (
                expected
            ), rows
            assert "last_candidate_completion=" in candidate.stdout
            assert "max_candidate_completion=" in candidate.stdout
            assert (
                "ordered_waw_overlap=enabled" if overlap
                else "ordered_waw_overlap=disabled"
            ) in candidate.stdout, candidate.stdout

        invalid_overlap = subprocess.run(
            [str(binary), str(two_mac), "1", "3", "7", "11",
             "4", "8", "--ordered-waw-overlap"],
            text=True, capture_output=True, check=False
        )
        assert invalid_overlap.returncode == 2, (
            invalid_overlap.stdout, invalid_overlap.stderr
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
