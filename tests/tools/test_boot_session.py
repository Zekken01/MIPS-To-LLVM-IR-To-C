#!/usr/bin/env python3
"""Unit tests for bounded, cache-preserving native EE boot sessions."""
import importlib.util
import io
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
             initial_function_count=None, expansion_per_pass=2,
             snapshots=False, quick_probe=False):
    with tempfile.TemporaryDirectory(prefix="ee-boot-session-") as temp:
        work = Path(temp) / "work"
        report = work / "first-boot.json"
        elf = Path(temp) / "synthetic.elf"
        elf.write_bytes(b"synthetic test input, never executed")
        calls = []
        if quick_probe:
            work.mkdir(parents=True)
            (work / "boot.log").write_text("previous full trace", encoding="utf-8")
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
            assert not report.exists(), "a previous pass's report survived"
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
            "--expansion-passes=2",
            f"--expansion-per-pass={expansion_per_pass}",
            "--driver", str(ROOT / "tools" / "run_game.py"),
            "--hle-startup", "--kseg-ram", "--max-dispatches=1000",
        ]
        if initial_reuse:
            argv.append("--no-recompile")
        if quick_probe:
            argv.extend(("--quick-probe", "--no-iop-catalog"))
        if snapshots:
            work.mkdir(parents=True, exist_ok=True)
            display = work / "gs-display.ppm"
            upload = work / "gs-upload.ppm"
            display.write_bytes(b"stale display")
            upload.write_bytes(b"stale upload")
            argv.extend((
                "--gs-snapshot", str(display),
                "--gs-upload-snapshot", str(upload),
            ))
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
        if quick_probe:
            assert all("--trace" not in call for call in calls), calls
            assert report.is_file()
            assert (work / "boot.log").read_text() == "previous full trace"
            log = (work / "quick-probe.log").read_text()
            return code, calls, log, log
        if len(passes) > 1:
            assert "--trace" in calls[-1], calls
        if initial_reuse:
            assert "--no-recompile" in calls[0], calls
        if snapshots:
            # Temporary expansion passes must not emit image artifacts;
            # the final replay is the only one with the two options.
            assert all(
                not any(arg.startswith("--gs-") for arg in call)
                for call in calls[:-1]
            ), calls
            assert f"--gs-snapshot={display.resolve()}" in calls[-1]
            assert f"--gs-upload-snapshot={upload.resolve()}" in calls[-1]
            # No emulated images were produced: old files were removed.
            assert not display.exists() and not upload.exists()
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


def test_ll_counter():
    # The progress bar belongs only to a real console and never writes
    # thousands of persistent log lines. Repeated polls have no output.
    class c_fake_terminal(io.StringIO):
        def isatty(self):
            return True

    with tempfile.TemporaryDirectory(prefix="ee-ll-progress-") as temp:
        ir_dir = Path(temp) / "ir"
        ir_dir.mkdir()
        terminal = c_fake_terminal()
        progress = SESSION.c_ll_file_progress(temp, stream=terminal)
        assert progress.m_enabled
        progress.refresh()  # No guessed or hardcoded denominator.
        assert terminal.getvalue() == ""

        (ir_dir / "ee_expected_ll_count.txt").write_text(
            "7550\n", encoding="ascii"
        )
        progress.refresh()
        progress.refresh()
        (ir_dir / "first.ll").write_text("define void @first() {}\n")
        progress.refresh()
        progress.refresh()
        (ir_dir / "second.ll").write_text("define void @second() {}\n")
        progress.refresh()
        before_done = terminal.getvalue()
        assert before_done.count("\r") == 3, repr(before_done)
        assert "\n" not in before_done, repr(before_done)
        assert "LLVM IR [0/7550]" in before_done
        assert "LLVM IR [1/7550]" in before_done
        assert "LLVM IR [2/7550]" in before_done
        progress.finish_line()
        assert terminal.getvalue().endswith("\n")
        progress.refresh()  # Preserve last display across sessions.
        assert terminal.getvalue().count("\r") == 3

        # In a pipe/CI log, no carriage returns or per-file updates.
        piped = io.StringIO()
        quiet = SESSION.c_ll_file_progress(temp, stream=piped)
        quiet.refresh()
        quiet.finish_line()
        assert piped.getvalue() == ""

        # If an old discovery marker is absent, use actual persisted
        # function-count metadata (cached --no-recompile sessions).
        (ir_dir / "ee_expected_ll_count.txt").unlink()
        (ir_dir / "ee_functions.json").write_text(
            json.dumps({"functions_discovered": 5}), encoding="utf-8"
        )
        alt = c_fake_terminal()
        cached = SESSION.c_ll_file_progress(temp, stream=alt)
        cached.refresh()
        assert "LLVM IR [2/5]" in alt.getvalue()
        cached.finish_line()

        # Dynamic functions beyond the original static discovery total
        # must never create an impossible [current > expected] ratio.
        (ir_dir / "ee_functions.json").write_text(
            json.dumps({"functions_discovered": 1}), encoding="utf-8"
        )
        dynamic = c_fake_terminal()
        updated = SESSION.c_ll_file_progress(temp, stream=dynamic)
        updated.refresh()
        assert "LLVM IR [2/2]" in dynamic.getvalue()
        updated.finish_line()


def test_windows_launcher_preserves_guest_cache():
    # The Windows launcher must try a safe host-only relink before any
    # checkout-triggered cache miss falls back to recompiling all guest IR.
    launcher = (ROOT / "run_ps2_game.bat").read_text(encoding="utf-8")
    probe = launcher.split(
        "rem Git checkout may rebuild host tools", 1
    )[1].split("echo === Starting extracted-disc", 1)[0]
    check = probe.index("--no-recompile --link-only")
    recovery = probe.index("--relink-existing --link-only")
    assert check < recovery, "must validate existing library first"
    assert probe.count('--emit-opt=0') == 2, "both probes use boot optimization"
    assert probe.count('--work-dir "%WORK_DIR%"') == 2, "same guest cache"
    assert probe.count('set "CACHE_FLAG=--no-recompile"') == 2
    assert "if not errorlevel 1 (" in probe, "failed relink must not reuse"
    assert "full IR rebuild required" in probe

    # The full retail replay must be able to continue genuine guest IOP
    # execution past the old fixed 100k ceiling; quick probes stay short.
    assert 'set "DEFAULT_MAX_DISPATCHES=100000"' in launcher
    assert 'set "DEFAULT_MAX_DISPATCHES=500000"' in launcher
    assert 'if not defined MAX_DISPATCHES set "MAX_DISPATCHES=%DEFAULT_MAX_DISPATCHES%"' in launcher
    assert '--max-dispatches=%MAX_DISPATCHES%' in launcher
    assert '--timeout-seconds=%BOOT_TIMEOUT_SECONDS%' in launcher
    assert '--max-dispatches=100000 ^' not in launcher
    assert 'if /I "%~1"=="--quick" set "SESSION_ARGS=--quick-probe --no-iop-catalog"' in launcher
    assert launcher.index('if not defined CACHE_FLAG set "SESSION_ARGS="') < launcher.index(
        'if /I "%~1"=="--quick"')
    assert launcher.index('if /I "%~1"=="--quick"') < launcher.index(
        'if /I "%~1"=="--full"')
    assert 'set "EE_WEBGPU_SNAPSHOT=0"' in launcher
    assert 'set "EE_WEBGPU_WINDOW=0"' in launcher
    assert launcher.index('set "EE_WEBGPU_SNAPSHOT=0"') < launcher.index(
        'if /I "%EE_WEBGPU_SNAPSHOT%"=="1"')
    assert launcher.index('set "EE_WEBGPU_WINDOW=0"') < launcher.index(
        'if /I "%EE_WEBGPU_WINDOW%"=="1"')


def test_guest_rpc_snapshots_in_cached_probe():
    # A long cached probe must retain read-only guest worker diagnostics
    # without writing one line for every EE dispatch.
    runner = (ROOT / "src" / "tools" /
              "ee_game_runner_main.cpp").read_text(encoding="utf-8")
    assert "if (report_path != nullptr &&\n            dispatched >= next_iop_guest_rpc_snapshot)" in runner
    assert "if (trace && report_path != nullptr &&\n            dispatched >= next_iop_guest_rpc_snapshot)" not in runner
    assert "IOP guest RPC pending registers:" in runner
    assert "IOP guest RPC pending stack:" in runner
    assert "index < 129u" in runner


def main():
    test_guest_rpc_snapshots_in_cached_probe()
    test_windows_launcher_preserves_guest_cache()
    test_ll_counter()
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 0, "exit_reason": 0, "dispatch_status": 1,
         "code": 4, "message": "DISPATCH LIMIT"},
    ], initial_reuse=True, initial_function_count=421, quick_probe=True)
    assert code == 4 and len(calls) == 1
    assert "--no-recompile" in calls[0] and "--auto-expand=2" in calls[0]
    assert "new_functions=0" in expansion and "DISPATCH LIMIT" in boot

    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2, "function_count": 423, "message": "CAP"},
        {"trace": False, "replay": 1, "function_count": 424, "exit_reason": 5,
         "message": "LOOP_BUDGET"},
    ], initial_reuse=True, initial_function_count=421, quick_probe=True)
    assert code == 3 and len(calls) == 2
    assert "new_functions=2" in expansion and "LOOP_BUDGET" in boot

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
    ], expansion_per_pass=128)
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
    ], initial_reuse=True, initial_function_count=129,
       expansion_per_pass=128)
    assert len(calls) == 3 and "new_functions=128" in expansion

    # Never retry unless the actual number of added functions hit cap.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 76, "initial_link": 1,
         "function_count": 100},
        {"trace": True, "replay": 0},
    ], expansion_per_pass=128)
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

    # Final-only GS diagnostics must not leak into 128-function
    # expansion passes, nor preserve previous session's misleading PPMs.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 1, "exit_reason": 5},
        {"trace": True, "replay": 0, "exit_reason": 5},
    ], initial_reuse=True, snapshots=True)
    assert code == 3 and "gs-snapshot" in boot
    assert "gs-snapshot" not in expansion

    # Even if the second pass also hits its cap, stop at the session
    # budget and preserve the translated DLL for next local invocation.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2},
        {"trace": False, "replay": 2},
        {"trace": True, "replay": 0, "message": "TRACE FINAL"},
    ], initial_reuse=True)
    assert code == 3
    assert len(calls) == 3

    # A failed driver must leave only current diagnostics in either mode.
    for quick_probe in (False, True):
        with tempfile.TemporaryDirectory(prefix="ee-boot-session-failure-") as temp:
            work = Path(temp) / "work"
            work.mkdir()
            report = work / ("quick-probe.json" if quick_probe else "first-boot.json")
            report.write_text(json.dumps({
                "schema": "ee-game-run-report-v1",
                "snapshot_kind": "stopped",
                "dispatches": 100000,
                "final_pc": 0x001BD58C,
            }), encoding="utf-8")
            elf = Path(temp) / "synthetic.elf"
            elf.write_bytes(b"fixture")
            calls = []

            def failing_run(command, **kwargs):
                calls.append(command)
                return SimpleNamespace(returncode=1, stdout="compilation failed\n")

            argv = [
                str(elf), "--work-dir", str(work), "--report", str(report),
                "--expansion-passes=2", "--expansion-per-pass=2",
                "--driver", str(ROOT / "tools" / "run_game.py"), "--no-iop-catalog",
            ]
            if quick_probe:
                argv.extend(("--quick-probe", "--no-recompile"))
            with patch.object(SESSION.subprocess, "run", side_effect=failing_run):
                code = SESSION.main(argv)
            assert code == 1 and len(calls) == 1
            assert not report.exists(), "a failed probe retained an old stop report"
            log = work / ("quick-probe.log" if quick_probe else "boot.log")
            summary = log.read_text()
            assert "compilation failed" in summary
            assert "dispatches=? stop=unknown" in summary

    print("boot session orchestration: PASS")


if __name__ == "__main__":
    main()
