#!/usr/bin/env python3
"""Cycle-neutral instruction order validator, with strict external reference gating."""

import importlib.util
import json
import sys
import tempfile
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_compare_ee_instruction_traces.py tool_path")
    source_path = Path(sys.argv[1]).resolve()
    spec = importlib.util.spec_from_file_location("compare_ee_instruction_traces", source_path)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    with tempfile.TemporaryDirectory(prefix="ee-instruction-reference-") as tmp:
        root = Path(tmp)
        native_path = root / "native.csv"
        native_path.write_text(
            "attempt,pc,opcode_word\n"
            "101,0x00100000,0x24020000\n"
            "108,0x00100008,0x8c620000\n"
            "109,0x0010000c,0x1040fffe\n"
            "110,0x00100010,0x00000000\n"
            "111,0x00100008,0x8c620000\n"
            "112,0x0010000c,0x1040fffe\n"
            "113,0x00100010,0x00000000\n"
            "# truncated=true range=0x00100000:0x00100010\n",
            encoding="utf-8",
        )
        report_path = root / "report.json"
        report_path.write_text(json.dumps({
            "guest_instruction_attempts": 200,
            "snapshot_kind": "stopped", "exit_name": "LOOP_BUDGET",
            "dispatches": 12, "final_pc": 0x00100008,
        }), encoding="utf-8")
        out_path = root / "summary.json"
        args = [str(native_path), "--report", str(report_path),
                "--loop-pcs", "0x00100008,0x0010000c,0x00100010",
                "--json", str(out_path)]
        assert tool.main(args) == 0
        summary = json.loads(out_path.read_text(encoding="utf-8"))
        assert summary["attempts_are_cycles"] is False
        assert summary["verified_captured_loop"]["captured_complete_iterations"] == 2
        assert summary["verified_captured_loop"]["prefix_entries"] == 1
        assert summary["terminal"]["attempts_after_last_captured"] == 87

        # Equal cycle timestamps are allowed for reference-verified dual issue;
        # the tool reports only supplied timestamps and does not extrapolate.
        reference_path = root / "independent.csv"
        reference_path.write_text(
            "cycle,pc,opcode_word\n"
            "10,0x00100000,0x24020000\n"
            "11,0x00100008,0x8c620000\n"
            "11,0x0010000c,0x1040fffe\n"
            "14,0x00100010,0x00000000\n"
            "15,0x00100008,0x8c620000\n"
            "15,0x0010000c,0x1040fffe\n"
            "18,0x00100010,0x00000000\n"
            "19,0x00100008,0x8c620000\n",
            encoding="utf-8",
        )
        reference_original = reference_path.read_text(encoding="utf-8")
        args_ref = args + ["--reference", str(reference_path),
                           "--reference-provenance", "test-fixture-not-hardware"]
        assert tool.main(args_ref) == 0
        checked = json.loads(out_path.read_text(encoding="utf-8"))
        assert checked["reference"]["records_compared"] == 7
        assert checked["reference"]["reference_matched_cycle_span"] == 8
        assert checked["reference"]["external_timing_independently_verified"] is False

        # Missing explicit provenance is a hard CLI failure.
        try:
            tool.main(args + ["--reference", str(reference_path)])
        except SystemExit as error:
            assert error.code == 2
        else:
            raise AssertionError("accepted anonymous reference timings")

        # A wrong opcode is never hidden by coincidentally matching PCs.
        reference_path.write_text(reference_path.read_text().replace(
            "0x0010000c,0x1040fffe", "0x0010000c,0x10400004", 1
        ), encoding="utf-8")
        assert tool.main(args_ref) == 1

        # Backwards reference cycles must be rejected rather than reordered.
        reference_path.write_text(reference_original.replace(
            "14,0x00100010", "9,0x00100010"
        ), encoding="utf-8")
        assert tool.main(args_ref) == 1

        # A missing footer, an unaligned PC or noncontiguous loop attempts
        # invalidates any cycle analysis rather than guessing a correction.
        original = native_path.read_text(encoding="utf-8")
        native_path.write_text(original.replace("# truncated=true range=0x00100000:0x00100010\n", ""), encoding="utf-8")
        assert tool.main(args) == 1
        native_path.write_text(original.replace("0x00100008", "0x00100009", 1), encoding="utf-8")
        assert tool.main(args) == 1
        native_path.write_text(original.replace("112,0x0010000c", "119,0x0010000c"), encoding="utf-8")
        assert tool.main(args) == 1

        # No reference means no synthetic EE cycles in the result at all.
        native_path.write_text(original, encoding="utf-8")
        assert tool.main(args) == 0
        no_ref = json.loads(out_path.read_text(encoding="utf-8"))
        assert "reference" not in no_ref
        assert "ee_cycles" not in no_ref

    print("EE native instruction trace validation: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
