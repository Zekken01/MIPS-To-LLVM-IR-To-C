#!/usr/bin/env python3
"""Exercise authentic ROM0 publication of SMFLAG through the native EE runner.

This test uses the same synthetic EE loop as the legacy IRX rendezvous test,
but the IOP is the official, pinned OpenPS2BIOS v0.6 ROM0 image. No IRX
compatibility service, preloaded SMFLAG, or synthetic DMA response is allowed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from test_game_runner import make_sif_handshake_elf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom0", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()

    source = Path(__file__).resolve().parents[2]
    rom0 = args.rom0.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    work = args.work_dir.resolve()
    report = args.report.resolve()
    log = args.log.resolve()
    if rom0.stat().st_size != 4 * 1024 * 1024:
        parser.error("ROM0 must be a 4 MiB image")
    work.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)

    elf = work / "ee-rom0-smflag.elf"
    make_sif_handshake_elf(elf)

    env = os.environ.copy()
    env["SDBZ_IOP_ROM0"] = str(rom0)
    # Verify the actual default, not a testing-only quantum override.
    env.pop("SDBZ_IOP_ROM0_QUANTUM", None)
    command = [
        sys.executable, str(source / "tools" / "run_game.py"),
        str(elf), "--build-dir", str(build),
        "--work-dir", str(work / "translated"),
        "--max-dispatches=1024", "--sif-poll-limit=256",
        "--timeout-seconds=180", "--hle-startup", "--trace",
        "--report", str(report),
    ]
    result = subprocess.run(
        command, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
        errors="replace", check=False, timeout=200,
    )
    log.write_text(result.stdout, encoding="utf-8")

    if not report.is_file():
        raise AssertionError(
            f"EE runner produced no report; exit={result.returncode}\n"
            + result.stdout[-8000:]
        )
    snapshot = json.loads(report.read_text(encoding="utf-8"))
    assert "IOP ROM0 reset: pc=0xbfc00000" in result.stdout, result.stdout[-8000:]
    assert "IOP ROM0 schedule: quantum=65536 " in result.stdout, result.stdout[-8000:]
    assert "IOP ROM0 compatibility bridges: disabled" in result.stdout, (
        result.stdout[-8000:]
    )
    assert "IOP guest published SMFLAG:" in result.stdout, result.stdout[-8000:]
    assert "SIF SMFLAG polling stalled" not in result.stdout, result.stdout[-8000:]
    assert (snapshot["gpr_lo"][16] & 0x00030000) == 0x00030000, snapshot
    assert snapshot["dispatches"] < 1024, snapshot
    print(
        "Official OpenPS2BIOS EE/IOP SMFLAG rendezvous: PASS "
        f"observed=0x{snapshot['gpr_lo'][16]:08x} "
        f"dispatches={snapshot['dispatches']} "
        f"exit={snapshot['exit_name']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
