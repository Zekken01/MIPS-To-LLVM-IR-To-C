#!/usr/bin/env python3
"""Unit tests for bounded, cache-preserving native EE boot sessions."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "run_boot_session", ROOT / "tools" / "run_boot_session.py",
)
SESSION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SESSION)


def run_case(passes, initial_reuse=False, diagnostics=False,
             initial_function_count=None):
    with tempfile.TemporaryDirectory(prefix="ee-boot-session-") as temp:
        work = Path(temp) / "work"
        report = work / "first-boot.json"
        elf = Path(temp) / "synthetic.elf"
        elf.write_bytes(b"synthetic test input, never executed")
        calls = []
        metadata = work / "ir" / "ee_functions.json"
        if initial_function_count is not None:
            metadata.parent.mkdir(parents=True, exist_ok=True)
            metadata.write_text(
                json.dumps({"function_count": initial_function_count}),
                encoding="utf-8",
            )

        def fake_run(command, **kwargs):
            assert kwargs["check"] is False
            assert kwargs["text"] is True
            assert kwargs["stderr"] is SESSION.subprocess.STDOUT
            calls.append(list(map(str, command)))
            trace = "--trace" in command
            index = len(calls) - 1
            item = passes[index]
            assert trace == item["trace"], (index, command, item)
            state = {
                "schema": "ee-game-run-report-v1",
                "dispatches": 140 + index * 40,
                "exit_reason": item.get("exit_reason", 3),
                "dispatch_status": item.get("dispatch_status", 0),
                "final_pc": 0x004E2640 + 16 * index,
            }
            report.write_text(json.dumps(state), encoding="utf-8")
            if "function_count" in item:
                metadata.parent.mkdir(parents=True, exist_ok=True)
                metadata.write_text(
                    json.dumps({"function_count": item["function_count"]}),
                    encoding="utf-8",
                )
            replay = item.get("replay")
            output = (
                f"== Executing translated EE code (replay {replay}) ==\\n"
                if replay is not None else ""
            ) + item.get("message", "EE stopped")
            if "initial_link" in item:
                output = (
                    f"link_native_objects: PASS ({item['initial_link']} functions, "
                    "fixture)\\n" + output
                )
            return SimpleNamespace(
                returncode=item.get("code", 3),
                stdout=output + "\\n",
            )

        argv = [
            str(elf), "--work-dir", str(work), "--report", str(report),
            "--expansion-passes=2", "--expansion-per-pass=2",
            "--driver", str(ROOT / "tools" / "run_game.py"),
            "--hle-startup", "--kseg-ram", "--max-dispatches=1000",
        ]
        if initial_reuse:
            argv.append("--no-recompile")
        if diagnostics:
            argv.extend((
                "--trace-entry=0x0018cf68",
                "--watch-word=0x01ffec14",
                "--trace-store=0x01ffec14",
                "--trace-v0=0x00194478:0x00194794",
            ))
        with patch.object(SESSION.subprocess, "run", side_effect=fake_run):
            code = SESSION.main(argv)
        assert len(calls) == len(passes), calls
        assert all("--trace" not in call for call in calls[:-1]), calls
        if len(passes) > 1:
            assert "--trace" in calls[-1], calls
        if initial_reuse:
            assert "--no-recompile" in calls[0], calls
        if diagnostics:
            assert all(
                "--trace-entry=0x0018cf68" in call and
                "--watch-word=0x01ffec14" in call and
                "--trace-store=0x01ffec14" in call and
                "--trace-v0=0x00194478:0x00194794" in call
                for call in calls
            ), calls
            assert "--trace-entry=0x0018cf68" in (
                work / "boot.log"
            ).read_text(), "missing diagnostic in boot command"
        if len(passes) > 2:
            assert "--no-recompile" in calls[1], calls
        assert "--auto-expand=0" in calls[-1] if len(passes) > 1 else True
        assert (work / "expansion.log").is_file()
        assert (work / "boot.log").is_file()
        return code, calls, (work / "expansion.log").read_text(), (work / "boot.log").read_text()


def main():
    # First pass consumes its expansion cap; the second finds a real
    # non-DISPATCH blocker and the final run emits the useful trace.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2, "message": "CAP"},
        {"trace": False, "replay": 1, "exit_reason": 1, "message": "SYSCALL"},
        {"trace": True, "replay": 0, "exit_reason": 1, "message": "TRACE FINAL"},
    ])
    assert code == 3
    assert "CAP" in expansion and "SYSCALL" in expansion, expansion
    assert "TRACE FINAL" not in expansion
    assert "TRACE FINAL" in boot
    assert "--no-recompile" not in calls[0]

    # Regression from the first PS2_GAME boot: 128 functions were added
    # in only 76 replays because jump-table siblings are batched.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 76, "initial_link": 1,
         "function_count": 129},
        {"trace": False, "replay": 4, "function_count": 138,
         "exit_reason": 1, "message": "REAL BLOCKER"},
        {"trace": True, "replay": 0, "exit_reason": 1,
         "message": "FINAL TRACE"},
    ])
    assert code == 3 and len(calls) == 3
    assert "new_functions=128" in expansion
    assert "new_functions=9" in expansion
    assert "--no-recompile" in calls[1]
    assert "FINAL TRACE" in boot

    # Cached passes measure relative to their existing function count.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 12, "function_count": 257},
        {"trace": False, "replay": 1, "function_count": 258,
         "exit_reason": 1},
        {"trace": True, "replay": 0, "exit_reason": 1},
    ], initial_reuse=True, initial_function_count=129)
    assert len(calls) == 3 and "new_functions=128" in expansion

    # Never retry unless the actual number of added functions hit cap.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 76, "initial_link": 1,
         "function_count": 100},
        {"trace": True, "replay": 0},
    ])
    assert len(calls) == 2 and "new_functions=99" in expansion

    # The SDBZ launcher forwards its diagnostic addresses through this
    # wrapper on both the expansion pass and the final traced replay.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 0, "exit_reason": 5},
        {"trace": True, "replay": 0, "exit_reason": 5},
    ], initial_reuse=True, diagnostics=True)
    assert code == 3
    assert "--watch-word=0x01ffec14" in expansion
    assert "--trace-store=0x01ffec14" in expansion
    assert "--trace-v0=0x00194478:0x00194794" in expansion
    assert "--trace-entry=0x0018cf68" in boot
    assert "--trace-store=0x01ffec14" in boot
    assert "--trace-v0=0x00194478:0x00194794" in boot

    # Even if the second pass also hits its cap, stop at the session
    # budget and preserve the translated DLL for next local invocation.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2},
        {"trace": False, "replay": 2},
        {"trace": True, "replay": 0, "message": "TRACE FINAL"},
    ], initial_reuse=True)
    assert code == 3
    assert len(calls) == 3

    # Never retry or hide an actual compilation failure. Copy its
    # output to boot.log, the file the Windows user normally uploads.
    with tempfile.TemporaryDirectory(prefix="ee-boot-session-failure-") as temp:
        work = Path(temp) / "work"
        report = work / "first-boot.json"
        elf = Path(temp) / "synthetic.elf"
        elf.write_bytes(b"fixture")
        calls = []

        def failing_run(command, **kwargs):
            calls.append(command)
            return SimpleNamespace(returncode=1, stdout="compilation failed\\n")

        argv = [
            str(elf), "--work-dir", str(work), "--report", str(report),
            "--expansion-passes=2", "--expansion-per-pass=2",
            "--driver", str(ROOT / "tools" / "run_game.py"),
        ]
        with patch.object(SESSION.subprocess, "run", side_effect=failing_run):
            code = SESSION.main(argv)
        assert code == 1 and len(calls) == 1
        assert "compilation failed" in (work / "boot.log").read_text()

    print("boot session orchestration: PASS")


if __name__ == "__main__":
    main()
