#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


EXPECTED_PROGRAM_SHA256 = (
    "27cd26fb9a1a1e05aa1d9b6986904e2b860d6afd8a2688ec4e2d5270da8fe3a8"
)
EXPECTED_PAIR_PCS = [
    0, 8, 16, 24, 32, 40, 48, 56, 64,
    80, 88, 104, 112, 120, 128, 136,
    168, 176, 192, 200, 208, 216, 224, 232, 240, 248,
    256, 264, 280, 288,
]


def executed_operation(lower):
    primary = (lower >> 25) & 0x3f
    if primary == 0x08:
        return "IADDIU"
    if primary == 0x09:
        return "ISUBIU"
    if primary == 0x3c:
        secondary = lower & 0x3f
        return {
            0x30: "IADD",
            0x31: "ISUB",
            0x32: "IADDI",
            0x34: "IAND",
            0x35: "IOR",
        }.get(secondary, "UNKNOWN")
    return {
        0x20: "B",
        0x21: "BAL",
        0x24: "JR",
        0x25: "JALR",
        0x28: "IBEQ",
        0x29: "IBNE",
        0x2c: "IBLTZ",
        0x2d: "IBGTZ",
        0x2e: "IBLEZ",
        0x2f: "IBGEZ",
    }.get(primary, "UNKNOWN")


class VU0CandidateTraceTests(unittest.TestCase):
    def test_candidate_trace_covers_fixture_and_comparator(self):
        if len(sys.argv) != 3:
            self.fail("expected candidate executable and comparator script paths")

        candidate_exe = Path(sys.argv[1]).resolve()
        comparator = Path(sys.argv[2]).resolve()
        self.assertTrue(candidate_exe.is_file(), candidate_exe)
        self.assertTrue(comparator.is_file(), comparator)

        with tempfile.TemporaryDirectory() as directory:
            trace_path = Path(directory) / "candidate.jsonl"
            result = subprocess.run(
                [str(candidate_exe), str(trace_path)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(trace_path.is_file())

            records = [
                json.loads(line)
                for line in trace_path.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]

            self.assertGreaterEqual(len(records), 2)
            header = records[0]
            steps = records[1:]
            self.assertEqual(header["schema"], "mips-to-llvm-vu0-trace-v1")
            self.assertEqual(header["kind"], "header")
            self.assertEqual(header["program_sha256"], EXPECTED_PROGRAM_SHA256)
            self.assertEqual(len(header["initial_state"]["vi"]), 32)
            self.assertEqual(header["initial_state"]["vi"][29], 1)
            self.assertEqual(len(steps), len(EXPECTED_PAIR_PCS))
            self.assertEqual([item["pc"] for item in steps], EXPECTED_PAIR_PCS)
            self.assertEqual(
                [item["step"] for item in steps],
                list(range(len(EXPECTED_PAIR_PCS))),
            )

            operations = {executed_operation(item["lower"]) for item in steps}
            self.assertNotIn("UNKNOWN", operations)
            self.assertEqual(
                operations,
                {
                    "IADDIU", "IADDI", "IADD", "ISUB", "IAND", "IOR",
                    "ISUBIU", "B", "BAL", "JR", "JALR", "IBEQ", "IBNE",
                    "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ",
                },
            )

            self.assertTrue(steps[4]["state"]["m_bit"])
            self.assertFalse(steps[5]["state"]["m_bit"])
            self.assertEqual(steps[7]["state"]["branch"], 1)
            self.assertEqual(steps[7]["state"]["branch_pc"], 80)

            final = steps[-1]["state"]
            self.assertEqual(final["vi"][1], 5)
            self.assertEqual(final["vi"][2], 4)
            self.assertEqual(final["vi"][3], 0x333)
            self.assertEqual(final["vi"][4], 1)
            self.assertEqual(final["vi"][5], 1)
            self.assertEqual(final["vi"][6], 5)
            self.assertEqual(final["vi"][7], 4)
            self.assertEqual(final["vi"][8], 36)
            self.assertEqual(final["vi"][10], 12)
            self.assertEqual(final["vi"][13], 16)
            self.assertEqual(final["vi"][15], 18)
            self.assertEqual(final["vi"][26], 296)
            self.assertEqual(final["vi"][29], 0)
            self.assertEqual(final["micro_pc"], 296)
            self.assertEqual(final["tpc"], 37)
            self.assertEqual(final["e_bit"], 0)
            self.assertFalse(final["running"])

            comparison = subprocess.run(
                [
                    sys.executable,
                    str(comparator),
                    str(trace_path),
                    str(trace_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(
                comparison.returncode,
                0,
                comparison.stdout + comparison.stderr,
            )
            self.assertIn("match exactly", comparison.stdout)


if __name__ == "__main__":
    unittest.main()
