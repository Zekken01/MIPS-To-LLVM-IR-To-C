#!/usr/bin/env python3
"""Run bounded auto-expansion sessions, then capture one diagnostic boot trace.

Keep this wrapper separate from run_game.py: changing the translation driver
invalidates its ELF/object provenance cache, while changing orchestration
must not throw away previously compiled guest functions.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


REPLAY_PATTERN = re.compile(r"== Executing translated EE code \(replay (\d+)\) ==")


def read_report(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return {}


def run_session(command, log_path, append=False):
    started = time.monotonic()
    result = subprocess.run(
        [str(item) for item in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    output = result.stdout or ""
    with log_path.open("a" if append else "w", encoding="utf-8") as log:
        log.write("+ " + subprocess.list2cmdline([str(item) for item in command]) + "\n")
        log.write(output)
    return result.returncode, output, time.monotonic() - started


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--expansion-passes", type=int, default=2)
    parser.add_argument("--expansion-per-pass", type=int, default=128)
    parser.add_argument("--no-recompile", action="store_true")
    parser.add_argument(
        "--driver", type=Path,
        default=Path(__file__).with_name("run_game.py"),
        help=argparse.SUPPRESS,
    )
    args, forwarded = parser.parse_known_args(argv)
    if not 1 <= args.expansion_passes <= 8:
        parser.error("--expansion-passes must be between 1 and 8")
    if not 1 <= args.expansion_per_pass <= 128:
        parser.error("--expansion-per-pass must be between 1 and 128")
    if any(item == "--trace" or item == "--link-only" or
           item == "--auto-expand" or item.startswith("--auto-expand=") or
           item == "--no-recompile" for item in forwarded):
        parser.error("trace, auto-expand, and no-recompile are managed by this wrapper")
    if not args.driver.is_file():
        parser.error(f"driver does not exist: {args.driver}")

    work = args.work_dir.resolve()
    report = args.report.resolve()
    work.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    expansion_log = work / "expansion.log"
    boot_log = work / "boot.log"
    expansion_log.write_text("", encoding="utf-8")
    base = [
        sys.executable, str(args.driver.resolve()), str(args.elf.resolve()),
        f"--work-dir={work}", f"--report={report}", *forwarded,
    ]

    reuse = args.no_recompile
    for index in range(args.expansion_passes):
        command = [*base, f"--auto-expand={args.expansion_per_pass}"]
        if reuse:
            command.append("--no-recompile")
        print(
            f"=== Expansion pass {index + 1}/{args.expansion_passes} "
            f"(up to {args.expansion_per_pass} new functions) ===",
            flush=True,
        )
        code, output, elapsed = run_session(command, expansion_log, append=True)
        state = read_report(report)
        replays = REPLAY_PATTERN.findall(output)
        last_replay = int(replays[-1]) if replays else -1
        final_pc = state.get("final_pc")
        pc_text = f"0x{final_pc:08x}" if isinstance(final_pc, int) else "unknown"
        print(
            f"pass {index + 1}: replay={last_replay} "
            f"dispatches={state.get('dispatches', '?')} "
            f"stop={pc_text} exit={code} elapsed={elapsed:.2f}s",
            flush=True,
        )

        if code not in (0, 3, 4):
            shutil.copyfile(expansion_log, boot_log)
            print(
                f"Expansion failed; diagnostics are in {boot_log}",
                file=sys.stderr, flush=True,
            )
            return code

        # A missing compiled guest function at the *last allowed replay*
        # means the driver hit its bound before it could append that PC.
        # Other failures (syscalls, non-executable PCs, timeouts or budget
        # exhaustion) must not be blindly retried.
        exhausted = (
            code == 3 and last_replay == args.expansion_per_pass and
            state.get("exit_reason") == 3 and
            state.get("dispatch_status") == 0 and
            isinstance(final_pc, int) and final_pc != 0
        )
        if exhausted and index + 1 < args.expansion_passes:
            print(
                "Expansion bound reached; resuming from the validated "
                "translation cache.", flush=True,
            )
            reuse = True
            continue
        if exhausted:
            print(
                "Session expansion budget reached; compiled functions "
                "are preserved for the next local run.", flush=True,
            )
        break

    # Emit the *full* guest trace once. Intermediate replays don't need
    # repetitive dispatch logs, but the final snapshot must stay useful
    # for diagnosing the next kernel service or translation failure.
    print(f"=== Final traced replay: {boot_log} ===", flush=True)
    code, _, elapsed = run_session(
        [*base, "--no-recompile", "--auto-expand=0", "--trace"],
        boot_log,
    )
    state = read_report(report)
    final_pc = state.get("final_pc")
    pc_text = f"0x{final_pc:08x}" if isinstance(final_pc, int) else "unknown"
    print(
        f"Final trace: dispatches={state.get('dispatches', '?')} "
        f"stop={pc_text} exit={code} elapsed={elapsed:.2f}s. "
        f"Logs: {boot_log}, {expansion_log}",
        flush=True,
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
