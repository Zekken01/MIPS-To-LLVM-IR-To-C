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
import struct

from test_game_runner import make_elf, make_sif_handshake_elf



def make_rom0_sifsetdma_elf(path):
    """Translated EE code waits for actual ROM0, then sends INIT_CMD via DMA6."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(0x600 - len(image)))
    struct.pack_into("<II", image, 52 + 16, 0x500, 0x500)
    program = (
        0x24040004,  # a0 = SIF_REG_SMFLAG
        0x2403007a,  # v1 = SifGetReg
        0x0000000c,  # syscall
        0x1040fffe,  # beq v0,zero,back to syscall
        0x00000000,  # branch delay slot
        0x24040002,  # a0 = SIF_REG_SUBADDR
        0x2403007a,  # v1 = SifGetReg
        0x0000000c,  # v0 = guest-published DMA10 receive buffer
        0x3c080010,  # lui t0,0x0010
        0x35080200,  # t0 = 0x00100200, descriptor list
        0xad020004,  # sw v0,4(t0), destination
        0x3c090010,  # lui t1,0x0010
        0x35290300,  # t1 = 0x00100300, request source
        0xad090000,  # sw t1,0(t0)
        0x240a0014,  # t2 = 20 packet bytes
        0xad0a0008,  # sw t2,8(t0), byte count
        0x240a0044,  # t2 = SIF_DMA_ERT | SIF_DMA_INT_O
        0xad0a000c,  # sw t2,12(t0), attributes
        0x01002021,  # addu a0,t0,zero
        0x24050001,  # a1 = one descriptor
        0x24030077,  # v1 = SifSetDma
        0x0000000c,  # syscall: real DMA6 -> real DMA10
        0x00408021,  # s0 = DMA ID
        0x00402021,  # a0 = DMA ID
        0x24030076,  # v1 = SifDmaStat
        0x0000000c,  # syscall
        0x00408821,  # s1 = -1 (completed)
        0x0000000d,  # BREAK terminates synthetic fixture
    )
    for index, word in enumerate(program):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    # One authentic SIFCMD INIT_CMD option-0 request, 20 bytes.
    # A host implementation must not replace the actual IOP command.
    struct.pack_into(
        "<5I", image, 0x400,
        20, 0, 0x80000002, 0, 0x00180000,
    )
    path.write_bytes(image)


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

    # The second ELF actually reaches the EE kernel SifSetDma syscall.
    # It must go through native DMA6 / IOP DMA10, not the disabled
    # compatibility bridge. The firmware must then execute its own code.
    dma_elf = work / "ee-rom0-sifsetdma.elf"
    dma_report = report.with_name("rom0-ee-sifsetdma.json")
    dma_log = log.with_name("rom0-ee-sifsetdma.log")
    make_rom0_sifsetdma_elf(dma_elf)
    dma_command = [
        sys.executable, str(source / "tools" / "run_game.py"),
        str(dma_elf), "--build-dir", str(build),
        "--work-dir", str(work / "translated-dma"),
        "--max-dispatches=1024", "--sif-poll-limit=256",
        "--timeout-seconds=180", "--hle-startup", "--trace",
        "--report", str(dma_report),
    ]
    dma_result = subprocess.run(
        dma_command, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
        errors="replace", check=False, timeout=200,
    )
    dma_log.write_text(dma_result.stdout, encoding="utf-8")
    assert dma_report.is_file(), dma_result.stdout[-8000:]
    dma_state = json.loads(dma_report.read_text(encoding="utf-8"))
    assert "IOP ROM0 compatibility bridges: disabled" in dma_result.stdout, (
        dma_result.stdout[-8000:]
    )
    assert "HLE SifSetDma: descriptors=0x00100200 " in dma_result.stdout, (
        dma_result.stdout[-8000:]
    )
    assert "real DMA6/FIFO/DMA10" in dma_result.stdout, (
        dma_result.stdout[-8000:]
    )
    assert "IOP ROM0 processed SIF1 turn:" in dma_result.stdout, (
        dma_result.stdout[-8000:]
    )
    assert "HLE SifDmaStat: id=1 status=-1" in dma_result.stdout, (
        dma_result.stdout[-8000:]
    )
    assert dma_state["gpr_lo"][16] == 1, dma_state
    assert dma_state["gpr_lo"][17] == 0xFFFFFFFFFFFFFFFF, dma_state
    assert "SifSetDma refused" not in dma_result.stdout, dma_result.stdout[-8000:]
    print(
        "Official OpenPS2BIOS EE SifSetDma syscall: PASS "
        "DMA6->DMA10 id=1 status=-1 "
        f"dispatches={dma_state['dispatches']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
