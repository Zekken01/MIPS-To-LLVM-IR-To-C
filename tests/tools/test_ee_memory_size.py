#!/usr/bin/env python3
"""Validate guest EE GetMemorySize without synthesizing boot or GPU progress."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

from test_game_runner import make_elf


def make_memory_size_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    words = (
        0x24020055,  # addiu v0,zero,0x55: must be replaced by size
        0x2403007f,  # addiu v1,zero,0x7f: GetMemorySize
        0x0000000c,  # syscall
        0x00408021,  # addu s0,v0,zero: 32 MiB
        0x2403007d,  # v1=0x7d: deliberately unsupported service
        0x0000000c,  # syscall: must fail closed
        0x00408821,  # s1=v0: must remain unreachable
        0x0000000d,  # break: must remain unreachable
    )
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
    elf = work / "memory-size.elf"
    report = work / "memory-size.json"
    make_memory_size_elf(elf)

    command = [
        sys.executable, str(source / "tools" / "run_game.py"), str(elf),
        "--build-dir", str(args.build_dir.resolve()),
        "--work-dir", str(work / "native"),
        "--functions=entry", "--max-dispatches=32",
        "--hle-startup", "--kseg-ram", "--trace",
        "--report", str(report),
    ]
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False, text=True, timeout=120,
    )
    (work / "memory-size.log").write_text(result.stdout)
    assert report.is_file(), result.stdout[-7000:]
    state = json.loads(report.read_text())
    assert "HLE GetMemorySize: bytes=0x02000000" in result.stdout, (
        result.stdout[-7000:]
    )
    assert state["gpr_lo"][16] == 32 * 1024 * 1024, state
    assert state["gpr_lo"][17] == 0, state
    assert state["exit_name"] == "EXCEPTION", state
    assert (state["cop0_cause"] >> 2) & 31 == 8, state
    assert state["cop0_epc"] == 0x0010001c, state
    print("EE GetMemorySize: PASS 32 MiB and unknown syscall fail-closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
