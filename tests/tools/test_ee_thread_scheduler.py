#!/usr/bin/env python3
"""Validate real native EE CreateThread/StartThread/preemption/ExitThread.

The synthetic MIPS worker writes guest RAM and signals a real HLE semaphore.
No game files, synthetic IRQs or IOP image are needed.
"""
from pathlib import Path
import argparse
import json
import struct
import subprocess
import sys

from test_game_runner import make_elf


def make_thread_elf(path, blocking=False, rotate=False):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(0x1100 - len(image)))
    struct.pack_into("<II", image, 52 + 16, 0x1000, 0x1000)
    # Main creates a zero-count semaphore and a dormant worker,
    # lowers its own priority, then starts worker ID 2. Worker runs
    # at priority 0 and signals the semaphore before exiting.
    main = (
        0x3c040010, 0x34840500, 0x24030040, 0x0000000c, 0x00408021,
        0x3c040010, 0x34840540, 0x24030020, 0x0000000c, 0x00408821,
        0x00002021, 0x24050001, 0x24030029, 0x0000000c,
        0x02202021, 0x24050042, 0x24030022, 0x0000000c,
        0x02002021, 0x24030044, 0x0000000c,
        0x3c080010, 0x35080580, 0x8d120000, 0x0000000d,
    )
    if blocking:
        # Do not lower main priority. It must explicitly block in WaitSema
        # before the lower-priority worker can run.
        main = list(main)
        main[10:14] = [0, 0, 0, 0]
        # Preserve the worker's observed waiter count in s3.
        main[-1:] = [
            0x3c080010, 0x35080584, 0x8d130000, 0x0000000d,
        ]
    if rotate:
        # Leave the initial and worker threads at the same priority.
        # StartThread alone cannot preempt the older running main.
        # RotateThreadReadyQueue(0) must move it behind the READY worker.
        main = list(main)
        main[10:14] = [0, 0, 0, 0]
        # First rotate a *different* priority (0x81 masks to 1):
        # this must return 1 without switching. Then rotate our own
        # priority 0 to yield to the READY worker.
        main[18:21] = [
            0x24040081, 0x2403002b, 0x0000000c,
            0x0040a021,  # move s4, v0: masked priority returned
            0x00002021, 0x2403002b, 0x0000000c,
        ]
    for index, word in enumerate(main):
        struct.pack_into("<I", image, 0x108 + index * 4, word)

    # Guest worker at 0x00100200: store the genuine StartThread argument
    # (0x42) to guest RAM, call SignalSema, then call ExitThread.
    worker = (
        0x3c080010, 0x35080580, 0xad040000,
        0x24040001, 0x24030042, 0x0000000c,
        0x24030023, 0x0000000c, 0x0000000d,
    )
    if blocking:
        # Inspect the real kernel semaphore queue before signaling main.
        # ReferSemaStatus writes ee_sema_t to 0x001005a0; the worker
        # copies wait_threads (+12) to shared RAM at 0x00100584.
        worker = (
            0x00808021,  # move s0, a0: preserve StartThread argument
            0x24040001, 0x3c050010, 0x34a505a0,
            0x24030047, 0x0000000c,
            0x3c080010, 0x350805ac, 0x8d090000,
            0x3c080010, 0x35080584, 0xad090000,
            0x02002021,  # move a0, s0: restore worker argument
        ) + worker
    for index, word in enumerate(worker):
        struct.pack_into("<I", image, 0x300 + index * 4, word)

    # ee_sema_t: count, max_count, init_count, wait_threads, attr, option.
    struct.pack_into("<6I", image, 0x600, 0, 1, 0, 0, 0, 0)
    # ee_thread_t: status, entry, stack, size, gp, initial priority,
    # current priority, attr, option. File-offset 0x640 => guest 0x100540.
    struct.pack_into(
        "<9I", image, 0x640,
        16, 0x00100200, 0x00100600, 0x400, 0,
        1 if blocking else 0, 0, 0, 0
    )
    path.write_bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    elf = work / "thread-scheduler.elf"
    report = work / "thread-scheduler.json"
    make_thread_elf(elf)
    command = [
        sys.executable, str(source / "tools" / "run_game.py"), str(elf),
        "--build-dir", str(args.build_dir.resolve()),
        "--work-dir", str(work / "native"),
        "--functions=entry", "--extra-functions=0x00100200",
        "--max-dispatches=256", "--hle-startup", "--kseg-ram",
        "--trace", "--report", str(report),
    ]
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False, timeout=120
    )
    (work / "thread-scheduler.log").write_text(result.stdout)
    assert report.is_file(), result.stdout[-6000:]
    state = json.loads(report.read_text())
    assert "HLE StartThread: id=2 entry=0x00100200 " in result.stdout, (
        result.stdout[-6000:]
    )
    assert "HLE EE thread switch: id=2 pc=0x00100200" in result.stdout, (
        result.stdout[-6000:]
    )
    assert "HLE SignalSema: id=1 " in result.stdout, result.stdout[-6000:]
    assert "HLE ExitThread: id=2" in result.stdout, result.stdout[-6000:]
    assert "HLE EE thread switch: id=1 " in result.stdout, result.stdout[-6000:]
    assert state["exit_name"] == "EXCEPTION", state
    assert (state["cop0_cause"] >> 2) & 31 == 9, state  # main BREAK
    assert state["gpr_lo"][18] == 0x42, state  # written by guest worker
    assert state["gpr_lo"][16] == 1, state  # CreateSema ID
    assert state["gpr_lo"][17] == 2, state  # CreateThread ID
    # Second fixture: main remains at priority 0, worker is priority 1.
    # A zero-count WaitSema must suspend main, then SignalSema must
    # wake the exact blocked guest thread and restore its context.
    blocking_elf = work / "thread-blocking.elf"
    blocking_report = work / "thread-blocking.json"
    make_thread_elf(blocking_elf, blocking=True)
    blocking_command = [
        sys.executable, str(source / "tools" / "run_game.py"),
        str(blocking_elf),
        "--build-dir", str(args.build_dir.resolve()),
        "--work-dir", str(work / "native-blocking"),
        "--functions=entry", "--extra-functions=0x00100200",
        "--max-dispatches=256", "--hle-startup", "--kseg-ram",
        "--trace", "--report", str(blocking_report),
    ]
    blocked = subprocess.run(
        blocking_command, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False, timeout=120
    )
    (work / "thread-blocking.log").write_text(blocked.stdout)
    assert blocking_report.is_file(), blocked.stdout[-6000:]
    blocked_state = json.loads(blocking_report.read_text())
    assert "HLE WaitSema: id=1 old_count=0 new_count=0" in blocked.stdout, (
        blocked.stdout[-6000:]
    )
    assert "HLE EE thread switch: id=2 pc=0x00100200" in blocked.stdout, (
        blocked.stdout[-6000:]
    )
    assert "HLE EE thread switch: id=1 " in blocked.stdout, (
        blocked.stdout[-6000:]
    )
    assert blocked_state["gpr_lo"][18] == 0x42, blocked_state
    assert "HLE ReferSemaStatus: id=1 " in blocked.stdout, (
        blocked.stdout[-6000:]
    )
    assert blocked_state["gpr_lo"][19] == 1, blocked_state  # one real waiter
    assert blocked_state["exit_name"] == "EXCEPTION", blocked_state
    assert (blocked_state["cop0_cause"] >> 2) & 31 == 9, blocked_state
    print("EE blocked WaitSema wakeup: PASS worker=2 signal=1 main_resumed=1")

    # Third fixture: RUN and READY at priority zero. A guest
    # RotateThreadReadyQueue(0) must dispatch the READY worker; merely
    # creating/starting the worker must not preempt the older main.
    rotate_elf = work / "thread-rotate.elf"
    rotate_report = work / "thread-rotate.json"
    make_thread_elf(rotate_elf, rotate=True)
    rotate_command = [
        sys.executable, str(source / "tools" / "run_game.py"),
        str(rotate_elf),
        "--build-dir", str(args.build_dir.resolve()),
        "--work-dir", str(work / "native-rotate"),
        "--functions=entry", "--extra-functions=0x00100200",
        "--max-dispatches=256", "--hle-startup", "--kseg-ram",
        "--trace", "--report", str(rotate_report),
    ]
    rotated = subprocess.run(
        rotate_command, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False, timeout=120
    )
    (work / "thread-rotate.log").write_text(rotated.stdout)
    assert rotate_report.is_file(), rotated.stdout[-6000:]
    rotated_state = json.loads(rotate_report.read_text())
    assert "HLE RotateThreadReadyQueue: priority=0 result=0" in rotated.stdout, (
        rotated.stdout[-6000:]
    )
    assert "HLE EE thread switch: id=2 pc=0x00100200" in rotated.stdout, (
        rotated.stdout[-6000:]
    )
    assert "HLE EE thread switch: id=1 " in rotated.stdout, (
        rotated.stdout[-6000:]
    )
    assert rotated_state["gpr_lo"][18] == 0x42, rotated_state
    assert rotated_state["gpr_lo"][20] == 1, rotated_state  # 0x81 -> 1
    assert rotated_state["exit_name"] == "EXCEPTION", rotated_state
    assert (rotated_state["cop0_cause"] >> 2) & 31 == 9, rotated_state
    print("EE RotateThreadReadyQueue: PASS equal_priority_worker_ran=1")

    print(
        "EE native thread scheduler: PASS "
        "worker=2 flag=0x42 sema=1 restored_main=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
