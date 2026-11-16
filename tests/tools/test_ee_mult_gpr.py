#!/usr/bin/env python3
"""Regress native R5900 MULT/MULTU destination-GPR and HI/LO semantics."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

from test_game_runner import make_elf


def make_mult_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<II", image, 52 + 16, 0x100, 0x100)

    def special(rs, rt, rd, funct):
        return (rs << 21) | (rt << 16) | (rd << 11) | funct

    words = [
        0x24040003,  # a0=3
        0x2405000c,  # a1=12
        special(4, 5, 3, 0x18),  # mult v1,a0,a1: v1=36
        special(3, 0, 16, 0x21),  # move s0,v1
        special(0, 0, 17, 0x12),  # mflo s1
        special(0, 0, 18, 0x10),  # mfhi s2
        0x2404fff9,  # a0=-7
        0x24050005,  # a1=5
        special(4, 5, 5, 0x18),  # mult a1,a0,a1: aliases rt
        special(5, 0, 19, 0x21),  # move s3,a1
        special(0, 0, 20, 0x12),  # mflo s4
        special(0, 0, 21, 0x10),  # mfhi s5
        0x3c04ffff,  # lui a0,0xffff
        0x3484ffff,  # ori a0,a0,0xffff
        0x24050002,  # a1=2
        special(4, 5, 4, 0x19),  # multu a0,a0,a1: aliases rs
        special(4, 0, 22, 0x21),  # move s6,a0
        special(0, 0, 23, 0x12),  # mflo s7
        special(0, 0, 8, 0x10),  # mfhi t0
        special(4, 5, 0, 0x18),  # mult zero,a0,a1: preserve r0
        0x0000000d,  # BREAK
    ]
    for index, opcode in enumerate(words):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[2]
    elf = work / "r5900-mult-gpr.elf"
    report = work / "r5900-mult-gpr.json"
    make_mult_elf(elf)
    command = [
        sys.executable, str(source / "tools" / "run_game.py"), str(elf),
        "--build-dir", str(args.build_dir.resolve()),
        "--work-dir", str(work / "native"), "--functions=entry",
        "--max-dispatches=64", "--loop-budget=100000",
        "--report", str(report),
    ]
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False, timeout=120,
    )
    (work / "r5900-mult-gpr.log").write_text(result.stdout)
    assert report.is_file(), result.stdout[-8000:]
    state = json.loads(report.read_text())
    assert state["exit_name"] == "EXCEPTION", state
    assert (state["cop0_cause"] >> 2) & 31 == 9, state
    gpr = state["gpr_lo"]
    mask = (1 << 64) - 1
    expected = {
        0: 0, 8: 1, 16: 36, 17: 36, 18: 0,
        19: (-35) & mask, 20: (-35) & mask, 21: mask,
        22: (-2) & mask, 23: (-2) & mask,
    }
    for reg, value in expected.items():
        assert gpr[reg] == value, (
            f"r{reg}: got 0x{gpr[reg]:016x} "
            f"expected 0x{value:016x}; {state}"
        )
    print("R5900 MULT/MULTU rd: PASS direct, rs/rt aliases, r0, HI/LO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
