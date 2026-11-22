#!/usr/bin/env python3
"""Offline tests for live cache-miss handoff; no game data or LLVM required."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ee_run_game_hot_expand", ROOT / "tools" / "run_game.py"
)
DRIVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRIVER)

# The same child process sends two requests and keeps one simulated piece of
# guest state ('cross_consumed'). A replay would reset that state to zero.
FAKE_RUNNER = r"""
from pathlib import Path
import sys
import time

folder = Path(sys.argv[1])
count = int(sys.argv[2])
cross_consumed = 1
for seq in range(1, count + 1):
    pc = 0x00348570 + (seq - 1) * 4
    (folder / f"request-{seq}.txt").write_text(
        f"{seq}\n0x{pc:08x}\n", encoding="utf-8"
    )
    deadline = time.monotonic() + 10
    response = folder / f"response-{seq}.txt"
    while not response.is_file() and time.monotonic() < deadline:
        time.sleep(0.02)
    if not response.is_file():
        sys.exit(7)
    # The native runner reads a binary file. The Python supervisor must
    # publish LF on Windows as well as Linux (no automatic CRLF conversion).
    if b"\r" in response.read_bytes():
        sys.exit(11)
    lines = response.read_text(encoding="utf-8").splitlines()
    if lines[0] != str(seq):
        sys.exit(8)
    if lines[1] == "STOP":
        print("expand_budget_stop")
        sys.exit(3)
    if lines[1].startswith("ERROR:"):
        print("expand_rejected")
        sys.exit(4)
    if not Path(lines[1]).is_file():
        sys.exit(9)
    if cross_consumed != 1:
        sys.exit(10)
print(f"resumed_without_replay cross_consumed={cross_consumed}")
"""


class TestLiveExpansion(unittest.TestCase):
    def run_fake(self, count, callback):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            output = io.StringIO()
            command = [sys.executable, "-c", FAKE_RUNNER, str(folder), str(count)]
            def compile_missing(pc, sequence):
                return callback(folder, pc, sequence)
            with contextlib.redirect_stdout(output):
                result = DRIVER.run_hot_expand(
                    command, folder, 5.0, compile_missing
                )
            return result, output.getvalue()

    def test_two_misses_resume_same_process_and_preserve_input(self):
        seen = []
        def compile_missing(folder, pc, sequence):
            seen.append((pc, sequence))
            library = folder / f"translated-{sequence}.dll"
            library.write_bytes(b"synthetic native library")
            return library
        result, output = self.run_fake(2, compile_missing)
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("resumed_without_replay cross_consumed=1", result.stdout)
        self.assertEqual(seen, [(0x00348570, 1), (0x00348574, 2)])

    def test_expansion_budget_returns_guest_stop(self):
        seen = []
        def compile_missing(folder, pc, sequence):
            seen.append(pc)
            if sequence == 2:
                return None
            library = folder / "translated-1.dll"
            library.write_bytes(b"synthetic")
            return library
        result, _ = self.run_fake(2, compile_missing)
        self.assertEqual(result.returncode, 3)
        self.assertIn("expand_budget_stop", result.stdout)
        self.assertEqual(seen, [0x00348570, 0x00348574])

    def test_compiler_error_is_fail_closed(self):
        def compile_missing(folder, pc, sequence):
            raise ValueError("IR verification failed")
        result, _ = self.run_fake(1, compile_missing)
        self.assertEqual(result.returncode, 4)
        self.assertIn("expand_rejected", result.stdout)


if __name__ == "__main__":
    unittest.main()
