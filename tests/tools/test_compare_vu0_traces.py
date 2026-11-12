#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "compare_vu0_traces.py"


def make_trace(engine, mutate=None):
    header = {
        "schema": "mips-to-llvm-vu0-trace-v1",
        "kind": "header",
        "engine": engine,
        "revision": "pinned-test-revision",
        "program_sha256": "a" * 64,
        "initial_state": {
            "vi": [0] * 32, "tpc": 0, "cycle": 0,
            "data_sha256": "0" * 64,
        },
    }
    state = {
        "vi": [0] * 32,
        "cycle": 1,
        "micro_pc": 8,
        "tpc": 1,
        "branch": 0,
        "branch_pc": 0,
        "delay_branch_pc": 0,
        "take_delay_branch": False,
        "vi_backup_cycles": 0,
        "vi_backup_register": 0,
        "vi_backup_old_value": 0,
        "e_bit": 0,
        "m_bit": 0,
        "vpu_status": 1,
        "running": True,
        "data_sha256": "0" * 64,
    }
    if mutate:
        mutate(header, state)
    return [
        header,
        {"kind": "step", "step": 0, "pc": 0, "lower": 0x3C000C30,
         "upper": 0, "state": state},
    ]


class VU0TraceComparatorTests(unittest.TestCase):
    def run_compare(self, left, right):
        with tempfile.TemporaryDirectory() as directory:
            left_path = Path(directory) / "left.jsonl"
            right_path = Path(directory) / "right.jsonl"
            for path, records in ((left_path, left), (right_path, right)):
                path.write_text(
                    "".join(json.dumps(item) + "\n" for item in records),
                    encoding="utf-8",
                )
            return subprocess.run(
                [sys.executable, str(SCRIPT), str(left_path), str(right_path)],
                text=True, capture_output=True, check=False,
            )

    def test_matching_traces_ignore_engine_metadata(self):
        result = self.run_compare(
            make_trace("pcsx2"), make_trace("candidate")
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("match exactly", result.stdout)

    def test_reports_individual_vi_register_mismatch(self):
        def change(_header, state):
            state["vi"][7] = 0x1234
        result = self.run_compare(
            make_trace("pcsx2"), make_trace("candidate", change)
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("VI7", result.stdout)
        self.assertIn("0x00000000 != 0x00001234", result.stdout)

    def test_reports_instruction_pc_mismatch(self):
        def change(_header, _state):
            pass
        reference = make_trace("pcsx2")
        candidate = make_trace("candidate")
        candidate[1]["pc"] = 8
        result = self.run_compare(reference, candidate)
        self.assertEqual(result.returncode, 1)
        self.assertIn("instruction pc", result.stdout)

    def test_rejects_missing_instruction_pc(self):
        reference = make_trace("pcsx2")
        candidate = make_trace("candidate")
        del candidate[1]["pc"]
        result = self.run_compare(reference, candidate)
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid pc", result.stderr)

    def test_reports_branch_and_cycle_mismatches(self):
        def change(_header, state):
            state["branch"] = 1
            state["cycle"] = 4
        result = self.run_compare(
            make_trace("pcsx2"), make_trace("candidate", change)
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("branch", result.stdout)
        self.assertIn("cycle", result.stdout)

    def test_reports_data_memory_mismatch(self):
        def change(_header, state):
            state["data_sha256"] = "1" * 64

        result = self.run_compare(
            make_trace("pcsx2"), make_trace("candidate", change)
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("data_sha256", result.stdout)

    def test_rejects_different_programs(self):
        def change(header, _state):
            header["program_sha256"] = "b" * 64
        result = self.run_compare(
            make_trace("pcsx2"), make_trace("candidate", change)
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("header mismatch: program_sha256", result.stderr)

    def test_rejects_missing_architectural_state(self):
        records = make_trace("pcsx2")
        del records[1]["state"]["vi_backup_cycles"]
        result = self.run_compare(records, make_trace("candidate"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("missing state fields", result.stderr)

    def test_rejects_trace_length_mismatch(self):
        left = make_trace("pcsx2")
        right = make_trace("candidate")
        right.append(dict(right[-1], step=1))
        result = self.run_compare(left, right)
        self.assertEqual(result.returncode, 1)
        self.assertIn("step count differs", result.stdout)


if __name__ == "__main__":
    unittest.main()
