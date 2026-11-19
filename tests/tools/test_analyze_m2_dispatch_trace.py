"""Tests for read-only M2 offline trace attribution."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from analyze_m2_dispatch_trace import analyze  # noqa: E402


class TestM2DispatchTrace(unittest.TestCase):
    def test_dedup_and_events(self):
        trace = (
            "dispatch=1 pc=0x00001000 -> 0x00001004 reason=RETURN(0) status=1\n"
            "dispatch=2 pc=0x00001004 -> 0x00001004 reason=HOST_YIELD(6) status=0\n"
            "EE host loop resume: count=1/256 pc=0x00001004\n"
            "dispatch=2 pc=0x00001004 -> 0x80000180 reason=EXCEPTION(1) status=0\n"
            "HLE INTC IRQ: cause=2 pending=0x00000004 handlers=1 epc=0x00001004\n"
            "dispatch=4 pc=0x00001004 -> 0x00001000 reason=RETURN(0) status=1\n"
            "IOP HLE RPC committed: seq=13 server=0x80000006 method=0x00000000 "
            "path=cdrom0:\\SIO2MAN.IRX;1 load_status=0 started=1\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "boot.log"
            path.write_text(trace, encoding="utf-8")
            report = {
                "dispatches": 4,
                "ee_cycle_source": "reference_compatibility",
                "gs_fields_started": 5,
                "scheduler_yields": 1,
            }
            analyzed = analyze(path, report, top=3)
        self.assertEqual(analyzed["trace_records"], 4)
        self.assertEqual(analyzed["distinct_dispatch_indices"], 3)
        self.assertEqual(analyzed["repeated_dispatch_indices"], 1)
        self.assertEqual(analyzed["indices_not_observed"], 1)
        self.assertEqual(analyzed["hot_pcs"][0]["pc"], "0x00001004")
        self.assertEqual(analyzed["hot_pcs"][0]["samples"], 2)
        self.assertEqual(analyzed["exit_reasons"], {"EXCEPTION": 1, "RETURN": 2})
        irq = analyzed["event_counts"]["intc_irq"]
        self.assertEqual(irq["count"], 1)
        self.assertEqual(irq["first"]["preceding_trace_dispatch"], 2)
        self.assertEqual(irq["first"]["fields"]["cause"], "2")
        self.assertEqual(analyzed["event_counts"]["committed_sif_rpc"]["count"], 1)
        self.assertEqual(analyzed["guest_clock"]["gs_fields_started"], 5)

    def test_record_limit_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "boot.log"
            path.write_text(
                "dispatch=1 pc=0x00001000 -> 0x00001004 reason=RETURN(0) status=1\n"
                "dispatch=2 pc=0x00001004 -> 0x00001008 reason=RETURN(0) status=1\n",
                encoding="utf-8")
            analyzed = analyze(path, {"dispatches": 2}, top=1, max_records=1)
        self.assertTrue(analyzed["record_limit_exceeded"])
        self.assertEqual(analyzed["distinct_dispatch_indices"], 1)
        self.assertEqual(analyzed["indices_not_observed"], 1)


if __name__ == "__main__":
    unittest.main()
