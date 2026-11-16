#!/usr/bin/env python3
"""Verify guest EE DMA5 callbacks using real OpenPS2BIOS SIF0 transport.

The synthetic EE ELF sends the same two INIT_CMD packets observed in the
Super Dragon Ball Z US boot trace. The IOP guest firmware, not the host,
builds and sends the 32-byte SIF0 reply. The registered EE DMAC callback
must receive channel/argument, set guest memory and preserve main-thread
registers. No fabricated response or interrupt is permitted.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from test_game_runner import make_elf


def lui(rt, value):
    return 0x3c000000 | (rt << 16) | (value & 0xffff)


def ori(rt, rs, value):
    return 0x34000000 | (rs << 21) | (rt << 16) | (value & 0xffff)


def addiu(rt, rs, value):
    return 0x24000000 | (rs << 21) | (rt << 16) | (value & 0xffff)


def sw(rt, offset, rs):
    return 0xac000000 | (rs << 21) | (rt << 16) | (offset & 0xffff)


def lw(rt, offset, rs):
    return 0x8c000000 | (rs << 21) | (rt << 16) | (offset & 0xffff)


def move(rd, rs):
    return (rs << 21) | (rd << 11) | 0x21


def beq(rs, rt, offset):
    return 0x10000000 | (rs << 21) | (rt << 16) | (offset & 0xffff)


def make_guest_dma5_callback_elf(path):
    make_elf(path)
    image = bytearray(0x900)
    original = path.read_bytes()
    image[:len(original)] = original
    struct.pack_into("<II", image, 52 + 16, 0x800, 0x800)

    # Handler 0x00100200; descriptor 0x00100300; flag 0x00100480.
    # Distinct packet sources reside at 0x00100600 and 0x00100620.
    program = [addiu(19, 0, 0x1234)]
    # Real zero-count guest semaphore. Its later count must come from
    # iSignalSema inside the authentic ROM0-triggered DMA5 callback.
    program += [
        lui(4, 0x0010), ori(4, 4, 0x0500),
        addiu(3, 0, 0x40), 0x0000000c,  # CreateSema
        move(21, 2),  # record allocated semaphore ID
    ]
    program += [
        addiu(4, 0, 5), lui(5, 0x0010), ori(5, 5, 0x0200),
        addiu(6, 0, 0), addiu(7, 0, 0x20),
        addiu(3, 0, 0x12), 0x0000000c,  # AddDmacHandler
        addiu(4, 0, 5), addiu(3, 0, 0x16),
        0x0000000c,                    # EnableDmac
        addiu(3, 0, 0x78), 0x0000000c,  # SifSetDChain
        addiu(4, 0, 4), addiu(3, 0, 0x7a),
        0x0000000c, beq(2, 0, -2), 0,  # Poll genuine SMFLAG
        addiu(4, 0, 2), addiu(3, 0, 0x7a),
        0x0000000c, move(18, 2),       # Genuine IOP SUBADDR
    ]

    for source, size in ((0x600, 20), (0x620, 16)):
        program += [
            lui(8, 0x0010), ori(8, 8, 0x0300),
            sw(18, 4, 8), lui(9, 0x0010), ori(9, 9, source),
            sw(9, 0, 8), addiu(10, 0, size), sw(10, 8, 8),
            addiu(10, 0, 0x44), sw(10, 12, 8),
            move(4, 8), addiu(5, 0, 1),
            addiu(3, 0, 0x77), 0x0000000c,  # Genuine SifSetDma
        ]

    program += [
        lui(11, 0x0010), ori(11, 11, 0x0480),
        lw(12, 0, 11), beq(12, 0, -2), 0,
        move(16, 12), move(17, 19),
        lw(18, 8, 11), lw(20, 12, 11),
        addiu(4, 0, 1), lui(5, 0x0010), ori(5, 5, 0x0540),
        addiu(3, 0, 0x47), 0x0000000c,  # ReferSemaStatus
        lui(8, 0x0010), ori(8, 8, 0x0540),
        lw(22, 0, 8),  # s6 observes genuine count=1
        0x0000000d,  # BREAK: deliberately end the guest fixture
    ]
    if 0x108 + len(program) * 4 >= 0x300:
        raise ValueError("EE entry overlaps its interrupt handler")
    for index, word in enumerate(program):
        struct.pack_into("<I", image, 0x108 + index * 4, word)

    # The genuine DMA5 callback must rearm SIF0 through the EE kernel's
    # interrupt-safe syscall -0x78 before it sets the completion flag.
    # It then captures a0/a1, changes s3 as a context-isolation probe,
    # and returns V0=0.
    handler = [
        addiu(3, 0, -0x78), 0x0000000c,  # iSifSetDChain
        lui(8, 0x0010), ori(8, 8, 0x0480),
        addiu(9, 0, 1), sw(9, 0, 8),
        sw(4, 8, 8), sw(5, 12, 8),
        addiu(4, 0, 1), addiu(3, 0, -0x43),
        0x0000000c,  # iSignalSema: genuine guest semaphore state
        addiu(19, 0, 0x7777), move(2, 0), 0x03e00008, 0,
    ]
    for index, word in enumerate(handler):
        struct.pack_into("<I", image, 0x300 + index * 4, word)

    # ee_sema_t: count, max_count, init_count, wait_threads, attr, option.
    struct.pack_into("<6I", image, 0x600, 0, 1, 0, 0, 0, 0)

    # Actual OpenPS2BIOS SIFCMD INIT_CMD option 0 then option 1.
    struct.pack_into("<5I", image, 0x700, 20, 0, 0x80000002, 0, 0x00180000)
    struct.pack_into("<4I", image, 0x720, 16, 0, 0x80000002, 1)
    path.write_bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom0", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    rom0 = args.rom0.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)

    elf = work / "ee-dma5-callback.elf"
    report = work / "ee-dma5-callback.json"
    log = work / "ee-dma5-callback.log"
    make_guest_dma5_callback_elf(elf)

    env = os.environ.copy()
    env["SDBZ_IOP_ROM0"] = str(rom0)
    env.pop("SDBZ_IOP_ROM0_QUANTUM", None)
    command = [
        sys.executable, str(source / "tools" / "run_game.py"),
        str(elf), "--build-dir", str(build),
        "--work-dir", str(work / "translation"),
        "--functions=entry", "--extra-functions=0x00100200",
        "--max-dispatches=1024", "--loop-budget=250000",
        "--sif-poll-limit=256", "--timeout-seconds=180",
        "--hle-startup", "--kseg-ram", "--trace",
        "--report", str(report),
    ]
    result = subprocess.run(
        command, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, errors="replace",
        check=False, timeout=200,
    )
    log.write_text(result.stdout, encoding="utf-8")
    assert report.is_file(), result.stdout[-10000:]
    snapshot = json.loads(report.read_text(encoding="utf-8"))

    assert "IOP ROM0 compatibility bridges: disabled" in result.stdout
    assert "HLE SifSetDma: descriptors=0x00100300 count=1 id=1 " in result.stdout
    assert "HLE SifSetDma: descriptors=0x00100300 count=1 id=2 " in result.stdout
    assert "sif1_delivered=48 sif0_delivered=32" in result.stdout
    assert "HLE DMAC IRQ: channel=5 pending=0x00000020 handlers=1" in result.stdout
    assert ("HLE DMAC callback enter: channel=5 handler=0x00100200 "
            "arg=0x00000020") in result.stdout
    assert ("HLE interrupt SIF syscall: pc=0x00100204 "
            "architectural_epc=0x001000cc resume=0x00100208") in result.stdout, (
        result.stdout[-6000:]
    )
    assert ("HLE iSifSetDChain: channel=5 qwc=0 "
            "chcr=0x00000184 result=void") in result.stdout
    assert result.stdout.count("HLE DMAC IRQ: channel=5 pending=") == 1
    assert "HLE iSignalSema: id=1 old_count=0 new_count=1 result=1" in result.stdout
    assert "HLE DMAC callback returned: channel=5 v0=0" in result.stdout
    assert snapshot["gpr_lo"][16] == 1, snapshot
    assert snapshot["gpr_lo"][17] == 0x1234, snapshot
    assert snapshot["gpr_lo"][18] == 5, snapshot
    assert snapshot["gpr_lo"][20] == 0x20, snapshot
    assert snapshot["gpr_lo"][21] == 1, snapshot  # CreateSema
    assert snapshot["gpr_lo"][22] == 1, snapshot  # ReferSemaStatus.count
    assert snapshot["dispatches"] < 1024, snapshot
    assert snapshot["exit_name"] == "EXCEPTION", snapshot
    assert (snapshot["cop0_cause"] & 0x7c) == 0x24, snapshot  # BREAK only
    assert "SifSetDma refused" not in result.stdout
    assert "guest loop budget exhausted" not in result.stdout
    print(
        "Official OpenPS2BIOS EE DMA5 callback: PASS "
        "SIF0=32 irq5=1 handler=0x00100200 "
        "flag=1 preserved_s3=0x1234 arg=0x20 "
        f"dispatches={snapshot['dispatches']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
