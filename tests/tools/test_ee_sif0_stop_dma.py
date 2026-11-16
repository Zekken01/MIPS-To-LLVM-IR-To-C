#!/usr/bin/env python3
"""Verify genuine EE SIF0 DMA CHCR stop and re-arm through guest syscalls."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

from test_game_runner import make_elf


def make_stop_dma_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x220 - len(image))))
    struct.pack_into("<II", image, 52 + 16, 0x120, 0x120)
    words = [
        0x3c081000,  # lui t0,0x1000
        0x3508c000,  # ori t0,t0,0xc000: EE SIF0 CHCR
        0x24020055,  # v0=0x55, both syscalls are void
        0x24030078,  # v1=sceSifSetDChain
        0x0000000c,  # syscall: arm SIF0
        0x8d100000,  # lw s0,0(t0): CHCR=0x184
        0x24040001,  # a0=1 (ignored by sceSifStopDma)
        0x2403006b,  # v1=sceSifStopDma
        0x0000000c,  # syscall: clear only CHCR.STR
        0x8d110000,  # lw s1,0(t0): CHCR=0x084
        0x00409021,  # move s2,v0: void call preserved 0x55
        0x8d130020,  # lw s3,0x20(t0): QWC=0
        0x24030078,  # v1=sceSifSetDChain
        0x0000000c,  # syscall: genuine re-arm
        0x8d140000,  # lw s4,0(t0): CHCR=0x184
        0x0000000d,  # guest BREAK
    ]
    for index, opcode in enumerate(words):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    elf = work / "sif0-stop-dma.elf"
    report = work / "sif0-stop-dma.json"
    make_stop_dma_elf(elf)
    result = subprocess.run(
        [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf), "--build-dir", str(args.build_dir.resolve()),
            "--work-dir", str(work / "native"),
            "--functions=entry", "--max-dispatches=32",
            "--hle-startup", "--kseg-ram", "--trace",
            "--report", str(report),
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False, text=True, timeout=120,
    )
    (work / "sif0-stop-dma.log").write_text(result.stdout)
    assert report.is_file(), result.stdout[-7000:]
    state = json.loads(report.read_text())
    assert state["exit_name"] == "EXCEPTION", state
    assert (state["cop0_cause"] >> 2) & 31 == 9, state
    gpr = state["gpr_lo"]
    assert gpr[16] == 0x184, state  # armed CHCR
    assert gpr[17] == 0x084, state  # stopped, mode/TIE preserved
    assert gpr[18] == 0x55, state   # void syscall V0 preserved
    assert gpr[19] == 0, state      # no phantom transfer
    assert gpr[20] == 0x184, state  # re-armed
    assert ("HLE SifStopDma: channel=5 old_chcr=0x00000184 "
            "new_chcr=0x00000084 result=void") in result.stdout, (
        result.stdout[-7000:]
    )
    print("EE SIF0 stop DMA: PASS CHCR.STR cleared, QWC unchanged, re-arm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
