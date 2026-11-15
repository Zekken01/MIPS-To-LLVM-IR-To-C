#!/usr/bin/env python3
"""Synthetic ELF32-to-native-runner integration test.

Runs a real MIPS ELF entry through the full emitter, object linker and
bounded native dispatcher. This is not a claim of PS2 BIOS bootability.
"""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile


def make_elf(path, startup=False):
    image = bytearray(0x140)
    image[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into(
        "<HHIIIIIHHHHHH", image, 16,
        2, 8, 1, 0x00100008, 52, 0, 0,
        52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<IIIIIIII", image, 52,
        1, 0x100, 0x00100000, 0x00100000,
        0x40, 0x60, 5, 16,
    )
    # Entry starts after two NOPs. ADDIU $v0,$zero,7;
    # JR $ra; NOP (delay slot).
    instructions = (
        (
            (0x108, 0x2405ffff), # a1 = -1: auto-allocate stack
            (0x10c, 0x24064000), # a2 = 16 KiB
            (0x110, 0x2403003c), # v1 = SetupThread
            (0x114, 0x0000000c), # syscall
            (0x118, 0x0040e821), # addu sp, v0, zero
            (0x11c, 0x03e00008), # jr ra
        ) if startup else (
            (0x108, 0x24020007),
            (0x10c, 0x03e00008),
        )
    )
    for offset, value in instructions:
        struct.pack_into("<I", image, offset, value)
    path.write_bytes(image)


def make_create_thread_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    # PT_LOAD guest descriptor at 0x00100200; CreateThread only
    # registers the dormant thread, it must never execute its entry.
    # The third call uses an unaligned descriptor and must fail closed.
    instructions = (
        (0x108, 0x3c040010),  # lui a0,0x0010
        (0x10c, 0x34840200),  # ori a0,a0,0x200
        (0x110, 0x24030020),  # v1 = CreateThread
        (0x114, 0x0000000c),  # syscall
        (0x118, 0x00404021),  # t0 = returned thread id 2
        (0x11c, 0x3c040010),  # lui a0,0x0010
        (0x120, 0x34840200),  # ori a0,a0,0x200
        (0x124, 0x24030020),  # v1 = CreateThread
        (0x128, 0x0000000c),  # syscall
        (0x12c, 0x00404821),  # t1 = returned thread id 3
        (0x130, 0x3c040010),  # lui a0,0x0010
        (0x134, 0x34840201),  # ori a0,a0,0x201 (unaligned)
        (0x138, 0x24030020),  # v1 = CreateThread
        (0x13c, 0x0000000c),  # syscall must be rejected
        (0x140, 0x240a0001),  # unreachable after invalid descriptor
        (0x144, 0x00000008),  # jr zero
        (0x148, 0x00000000),
    )
    for offset, opcode in instructions:
        struct.pack_into("<I", image, offset, opcode)
    descriptor = (
        0x00000000,  # initial slot
        0x001000f0,  # thread entry (not executed)
        0x00102000,  # stack base
        0x00000800,  # stack size
        0x0010f000,  # gp
        0x00000040,  # priority
        0x01020304,  # reserved field
        0x0000abcd,  # attr
        0xaabbccdd,  # option
    )
    struct.pack_into("<9I", image, 0x300, *descriptor)
    path.write_bytes(image)


def make_osd_config_elf(path, invalid_set=False):
    """Get/SetOsdConfigParam use guest pointers to one 32-bit OSD word."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    # Preserve the test code and three OSD words inside the PT_LOAD
    # segment, which begins at image offset 0x100.
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    # Guest OSD pointers: 0x00100200 (default GET),
    # 0x00100210 (SET input), 0x00100220 (persisted GET).
    instructions = (
        0x3c040010,  # lui a0,0x0010
        0x34840200,  # ori a0,a0,0x0200
        0x2403004b,  # v1 = GetOsdConfigParam
        0x0000000c,  # syscall
        0x8c880000,  # lw t0,0(a0): default configuration 0
        0x00404821,  # addu t1,v0,zero: success = 0
        0x3c040010,
        0x34840210,  # a0 = 0x00100210
        0x2403004a,  # v1 = SetOsdConfigParam
        0x0000000c,
        0x00405021,  # t2 = setter return 0
        0x3c040010,
        0x34840220,  # a0 = 0x00100220
        0x2403004b,  # v1 = GetOsdConfigParam
        0x0000000c,
        0x8c8b0000,  # lw t3,0(a0): configured word
        0x00406021,  # t4 = getter return 0
        0x3c040010 if invalid_set else 0x3c047fff,
        0x34840221 if invalid_set else 0x3484fffc,
                     # invalid SET: unaligned 0x00100221
                     # invalid GET: unmapped 0x7ffffffc
        0x2403004a if invalid_set else 0x2403004b,
        0x0000000c,  # must refuse invalid pointer, no write
        0x240d0001,  # unreachable after rejected syscall
        0x00000008,  # jr zero
        0x00000000,
    )
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    struct.pack_into("<I", image, 0x300, 0xcafebabe)
    struct.pack_into("<I", image, 0x310, 0x12345678)
    struct.pack_into("<I", image, 0x320, 0xfeedbabe)
    path.write_bytes(image)


def make_thread_priority_elf(path):
    """Exercise EE thread-priority return values and per-thread state."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    # PT_LOAD descriptor at guest VA 0x00100200.
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    instructions = (
        0x2403002f,  # GetThreadId
        0x0000000c,
        0x00404021,  # t0 = running ID 1
        0x24040001,  # a0 = main thread ID 1
        0x24050001,  # a1 = priority 1
        0x24030029,  # ChangeThreadPriority
        0x0000000c,
        0x00404821,  # t1 = previous priority 0
        0x24040000,  # a0 = current thread alias 0
        0x24050005,  # a1 = priority 5
        0x24030029,
        0x0000000c,
        0x00405021,  # t2 = previous priority 1
        0x3c040010,  # a0 = 0x00100200
        0x34840200,
        0x24030020,  # CreateThread from priority-64 descriptor
        0x0000000c,
        0x00405821,  # t3 = new dormant ID 2
        0x24040002,  # a0 = dormant thread ID 2
        0x24050082,  # a1 = 130, masked to 2
        0x2403002a,  # iChangeThreadPriority
        0x0000000c,
        0x00406021,  # t4 = old dormant priority 64
        0x24040002,
        0x24050007,  # new priority 7
        0x24030029,
        0x0000000c,
        0x00406821,  # t5 = old priority 2, proves write occurred
        0x24040063,  # a0 = invalid thread ID 99
        0x24050003,
        0x24030029,
        0x0000000c,
        0x00407021,  # t6 = -1
        0x2403002f,  # GetThreadId again, without scheduling child
        0x0000000c,
        0x00407821,  # t7 = running ID 1
        0x00000008,  # JR zero: synthetic exit
        0x00000000,
    )
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + 4 * index, word)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 64, 0, 0, 0
    )
    path.write_bytes(image)


def make_get_thread_id_elf(path):
    """GetThreadId must return the running main thread, not a dormant child."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    # The test's guest thread descriptor is at VA 0x00100200.
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)
    instructions = (
        (0x108, 0x2403002f),  # v1 = GetThreadId
        (0x10c, 0x0000000c),  # syscall
        (0x110, 0x00404021),  # t0 = GetThreadId before CreateThread
        (0x114, 0x3c040010),  # a0 = 0x00100200
        (0x118, 0x34840200),
        (0x11c, 0x24030020),  # v1 = CreateThread
        (0x120, 0x0000000c),  # syscall
        (0x124, 0x00404821),  # t1 = created dormant thread ID
        (0x128, 0x2403002f),  # v1 = GetThreadId again
        (0x12c, 0x0000000c),  # syscall (a0 still holds 0x00100200)
        (0x130, 0x00405021),  # t2 = GetThreadId after creation
        (0x134, 0x00000008),  # jr zero: finish bounded synthetic test
        (0x138, 0x00000000),
    )
    for offset, word in instructions:
        struct.pack_into("<I", image, offset, word)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 64, 0, 0, 0,
    )
    path.write_bytes(image)


def make_semaphore_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    if len(image) < 0x1a0:
        image.extend(bytes(0x1a0 - len(image)))
    # PT_LOAD includes the guest ee_sema_t at virtual address 0x00100080.
    struct.pack_into("<I", image, 52 + 16, 0xa0)
    struct.pack_into("<I", image, 52 + 20, 0xa0)
    for offset, value in (
        (0x108, 0x3c040010), # lui a0,0x0010
        (0x10c, 0x34840080), # ori a0,a0,0x0080
        (0x110, 0x24030040), # v1 = CreateSema
        (0x114, 0x0000000c), # syscall
        (0x118, 0x03e00008), # jr ra
        (0x11c, 0x00000000), # nop
    ):
        struct.pack_into("<I", image, offset, value)
    struct.pack_into("<IIIIII", image, 0x180, 0, 2, 1, 0, 0, 0)
    path.write_bytes(image)


def make_set_syscall_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    for offset, value in (
        (0x108, 0x24040083), # a0 = 0x83
        (0x10c, 0x3c050010), # lui a1,0x0010
        (0x110, 0x34a50030), # handler = 0x00100030
        (0x114, 0x24030074), # v1 = SetSyscall
        (0x118, 0x0000000c), # syscall
        (0x11c, 0x03e00008), # jr ra
        (0x120, 0x00000000), # delay slot
    ):
        struct.pack_into("<I", image, offset, value)
    path.write_bytes(image)


def make_eret_target_elf(path, status):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x300 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x200)
    struct.pack_into("<I", image, 52 + 20, 0x200)

    # Both alternate targets are reachable static basic blocks, but the
    # never-taken BNE instructions do not actually take either branch.
    # This tests the real MTC0+ERET instructions in LLVM-generated code.
    instructions = (
        (0x108, 0x1400001d),  # bne zero, zero, ErrorEPC 0x00100080
        (0x10c, 0x00000000),
        (0x110, 0x1400002b),  # bne zero, zero, EPC 0x001000c0
        (0x114, 0x00000000),
        (0x118, 0x24080000 | status),  # t0 = Status (EXL or ERL)
        (0x11c, 0x40886000),  # mtc0 t0, Status
        (0x120, 0x3c090010),  # lui t1, 0x0010
        (0x124, 0x35290080),  # ErrorEPC = 0x00100080
        (0x128, 0x4089f000),  # mtc0 t1, ErrorEPC
        (0x12c, 0x3c0a0010),  # lui t2, 0x0010
        (0x130, 0x354a00c0),  # EPC = 0x001000c0
        (0x134, 0x408a7000),  # mtc0 t2, EPC
        (0x138, 0x42000018),  # ERET
        (0x13c, 0x240f0001),  # must never execute
        (0x180, 0x400b6000),  # mfc0 t3, Status at ErrorEPC
        (0x184, 0x400cf000),  # mfc0 t4, ErrorEPC
        (0x188, 0x400d7000),  # mfc0 t5, EPC
        (0x18c, 0x00000008),  # jr zero to finish
        (0x190, 0x00000000),
        (0x1c0, 0x400b6000),  # mfc0 t3, Status at EPC
        (0x1c4, 0x400cf000),  # mfc0 t4, ErrorEPC
        (0x1c8, 0x400d7000),  # mfc0 t5, EPC
        (0x1cc, 0x00000008),
        (0x1d0, 0x00000000),
    )
    for offset, opcode in instructions:
        struct.pack_into("<I", image, offset, opcode)
    path.write_bytes(image)


def make_overlapping_entry_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    # Call at 0x00100050 first: its translated SYSCALL block is
    # followed by a return block owned at 0x00100058. Then compute a
    # runtime JALR target of 0x00100054, the *interior* SYSCALL PC.
    # Translating that entry must retain the existing owner of 0x100058,
    # and both calls must execute genuine native MIPS/SYSCALL behavior.
    # Including the syscall return in the target computation prevents
    # static call-target discovery from splitting the first function.
    instructions = (
        (0x108, 0x0c040014),  # jal 0x00100050
        (0x10c, 0x00000000),  # delay slot
        (0x110, 0x00408021),  # addu s0, v0, zero
        (0x114, 0x24030064),  # addiu v1, zero, FlushCache
        (0x118, 0x3c080010),  # lui t0, 0x0010
        (0x11c, 0x25080054),  # addiu t0, t0, 0x54
        (0x120, 0x01024021),  # addu t0, t0, v0
        (0x124, 0x0100f809),  # jalr ra, t0
        (0x128, 0x00000000),  # delay slot
        (0x12c, 0x00404821),  # addu t1, v0, zero
        (0x130, 0x00000008),  # jr zero (known stop)
        (0x134, 0x00000000),  # delay slot
        (0x150, 0x24030064),  # addiu v1, zero, FlushCache
        (0x154, 0x0000000c),  # SYSCALL (second entry)
        (0x158, 0x03e00008),  # jr ra (shared return block)
        (0x15c, 0x00000000),  # delay slot
    )
    for offset, opcode in instructions:
        struct.pack_into("<I", image, offset, opcode)
    path.write_bytes(image)


def make_interrupt_mask_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x500 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x400)
    struct.pack_into("<I", image, 52 + 20, 0x400)

    # Exercise both regular and interrupt-context syscall variants.
    # The return value is 1 only if a hardware mask bit changes.
    # Read actual EE INTC_MASK and DMAC D_STAT afterward to verify
    # the syscalls affect emulated guest-visible MMIO state.
    calls = (
        (11, 0x14, 8),   # EnableIntc -> 1
        (11, 0x14, 9),   # EnableIntc again -> 0
        (11, 0x15, 10),  # DisableIntc -> 1
        (11, 0x15, 11),  # DisableIntc again -> 0
        (11, 0x1a, 12),  # iEnableIntc -> 1
        (11, 0x1b, 13),  # iDisableIntc -> 1
        (2,  0x16, 14),  # EnableDmac -> 1
        (2,  0x16, 15),  # EnableDmac again -> 0
        (2,  0x17, 16),  # DisableDmac -> 1
        (2,  0x17, 17),  # DisableDmac again -> 0
        (2,  0x1c, 18),  # iEnableDmac -> 1
        (2,  0x1d, 19),  # iDisableDmac -> 1
        (11, 0x14, 20),  # finally enable INTC
        (2,  0x16, 21),  # finally enable DMAC
    )
    words = []
    for cause, service, dst in calls:
        words.extend((
            0x24040000 | cause,       # addiu a0,zero,cause
            0x24030000 | service,     # addiu v1,zero,service
            0x0000000c,              # syscall
            0x00400021 | (dst << 11),  # addu saved_register,v0,zero
        ))
    words.extend((
        0x3c1a1000,  # lui k0,0x1000
        0x375af010,  # ori k0,k0,INTC_MASK low
        0x8f560000,  # lw s6,0(k0)
        0x3c1a1000,  # lui k0,0x1000
        0x375ae010,  # ori k0,k0,D_STAT low
        0x8f570000,  # lw s7,0(k0)
        0x00000008,  # jr zero
        0x00000000,
    ))
    for index, opcode in enumerate(words):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_interrupt_handlers_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    # Registers: t0/t1/t2 receive global IDs 1/2/3; t3/t4/t5
    # receive remaining-handler counts. s0 checks remove-missing.
    # Exercise the guest syscall trap and resume, not just a decoder.
    instructions = (
        0x3c1c1234,  # lui gp,0x1234
        0x2404000b,  # a0 = INTC cause 11
        0x3c050010,  # a1 = 0x00100090
        0x34a50090,
        0x2406ffff,  # a2 = -1 (append)
        0x24070066,  # a3 = callback argument
        0x24030010,  # AddIntcHandler
        0x0000000c,
        0x00404021,  # t0 = handler ID 1
        0x2404000b,
        0x3c050010,
        0x34a500a0,  # a1 = 0x001000a0
        0x24060000,  # a2 = 0 (insert before first)
        0x24070077,
        0x24030010,  # AddIntcHandler
        0x0000000c,
        0x00404821,  # t1 = handler ID 2
        0x24040002,  # a0 = DMAC channel 2
        0x3c050010,
        0x34a500b0,  # a1 = 0x001000b0
        0x2406ffff,
        0x24070088,
        0x24030012,  # AddDmacHandler (shared global IDs)
        0x0000000c,
        0x00405021,  # t2 = handler ID 3
        0x2404000b,
        0x01202821,  # a1 = t1 (ID 2)
        0x24030011,  # RemoveIntcHandler
        0x0000000c,
        0x00405821,  # t3 = remaining 1
        0x2404000b,
        0x24050063,  # remove unknown ID 99
        0x24030011,
        0x0000000c,
        0x00408021,  # s0 = remaining 1, not success/failure
        0x2404000b,
        0x01002821,  # a1 = t0 (ID 1)
        0x24030011,
        0x0000000c,
        0x00406021,  # t4 = remaining 0
        0x24040002,
        0x01402821,  # a1 = t2 (ID 3)
        0x24030013,  # RemoveDmacHandler
        0x0000000c,
        0x00406821,  # t5 = remaining 0
        0x00000008,  # jr zero: finish through native dispatcher
        0x00000000,
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_out_of_range_set_syscall_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    # Install a valid service, then attempt out-of-range 0x12C.
    # Both valid entry 0x83 and the first word beyond the table
    # must remain intact. The second bad index tests overflow.
    instructions = (
        0x3c0a8000,  # lui t2, 0x8000 (KSEG0 base)
        0x24040083,  # addiu a0, zero, 0x83
        0x3c050010,  # lui a1, 0x0010
        0x34a50050,  # ori a1, a1, 0x0050
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d481a0c,  # lw t0, 0x1A0C(t2): original entry
        0x2404012c,  # addiu a0, zero, 0x12C
        0x3c058007,  # lui a1, 0x8007
        0x34a56488,  # ori a1, a1, 0x6488
        0x24020007,  # addiu v0, zero, 7 (overwritten)
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall: out-of-range index
        0x00404821,  # addu t1, v0, zero: zero return
        0x8d4b1a0c,  # lw t3, 0x1A0C(t2): valid entry still set
        0x8d4c1c00,  # lw t4, 0x1C00(t2): first word after table
        0x3c04ffff,  # lui a0, 0xFFFF
        0x3484ffff,  # ori a0, a0, 0xFFFF
        0x24020009,  # addiu v0, zero, 9 (overwritten)
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall: 0xFFFFFFFF
        0x00406821,  # addu t5, v0, zero
        0x8d4e1a0c,  # lw t6, 0x1A0C(t2): preserved
        0x8d4f1c00,  # lw t7, 0x1C00(t2): preserved
        0x00000008,  # jr zero
        0x00000000,  # nop
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_guest_syscall_table_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # Keep the complete synthetic program and both handler addresses
    # inside the single executable PT_LOAD region.
    image.extend(bytes(max(0, 0x1a0 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0xa0)
    struct.pack_into("<I", image, 52 + 20, 0xa0)

    # The HLE syscall table lives in physical RAM at 0x1800, and the
    # KSEG0 mirror begins at 0x80001800. Register 0x83 then 0x5A,
    # verify that their entries occupy separate 32-bit slots, replace
    # 0x83, and check that the adjacent, unwritten slot remains zero.
    instructions = (
        0x3c0a8000,  # lui t2, 0x8000 (KSEG0 base)
        0x8d481a0c,  # lw t0, 0x1A0C(t2): initially zero
        0x24040083,  # addiu a0, zero, 0x83
        0x3c050010,  # lui a1, 0x0010
        0x34a50050,  # ori a1, a1, 0x0050
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d491a0c,  # lw t1, 0x1A0C(t2): 0x00100050
        0x8c0b1a0c,  # lw t3, 0x1A0C(zero): physical alias
        0x2404005a,  # addiu a0, zero, 0x5A
        0x3c050010,  # lui a1, 0x0010
        0x34a50030,  # ori a1, a1, 0x0030
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d4c1968,  # lw t4, 0x1968(t2): 0x00100030
        0x8d4d1a0c,  # lw t5, 0x1A0C(t2): 0x00100050
        0x24040083,  # addiu a0, zero, 0x83
        0x3c050010,  # lui a1, 0x0010
        0x34a50030,  # ori a1, a1, 0x0030
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d4e1a0c,  # lw t6, 0x1A0C(t2): overwritten 0x00100030
        0x8d4f1a10,  # lw t7, 0x1A10(t2): unwritten 0x84, zero
        0x00000008,  # jr zero: stop after returning to caller
        0x00000000,  # nop (delay slot)
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_registered_callback_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(0x180 - len(image)))
    # Provide 0x80 file-backed bytes beginning at the PT_LOAD's 0x100 offset.
    struct.pack_into("<I", image, 52 + 16, 0x80)
    struct.pack_into("<I", image, 52 + 20, 0x80)
    # SetSyscall(0x83, 0x00100050), invoke it with a0=7, then preserve
    # the guest callback's return value in t0.
    for offset, value in (
        (0x108, 0x24040083),  # addiu a0, zero, 0x83
        (0x10c, 0x3c050010),  # lui a1, 0x10
        (0x110, 0x34a50050),  # ori a1, a1, 0x50
        (0x114, 0x24030074),  # addiu v1, zero, SetSyscall
        (0x118, 0x0000000c),  # syscall
        (0x11c, 0x24040007),  # addiu a0, zero, 7
        (0x120, 0x24030083),  # addiu v1, zero, 0x83
        (0x124, 0x0000000c),  # syscall -> installed callback
        (0x128, 0x00404021),  # addu t0, v0, zero
        (0x12c, 0x00000008),  # jr zero
        (0x130, 0x00000000),  # nop delay slot
        (0x150, 0x24820005),  # addiu v0, a0, 5
        (0x154, 0x03e00008),  # jr ra -> synthetic callback sentinel
        (0x158, 0x00000000),  # nop delay slot
    ):
        struct.pack_into("<I", image, offset, value)
    path.write_bytes(image)


def make_flush_cache_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # Two recognized cache services followed by an unsupported
    # service. A missing --hle-startup must reject the very first
    # service; enabled HLE must resume past both and stop at 0x65.
    instructions = (
        0x2402007b,  # addiu v0, zero, 123: prove return overwrites v0
        0x24030064,  # addiu v1, zero, 0x64 (FlushCache)
        0x0000000c,  # syscall
        0x00404021,  # addu t0, v0, zero: expected 0
        0x2402007c,  # addiu v0, zero, 124
        0x24030068,  # addiu v1, zero, 0x68 (iFlushCache)
        0x0000000c,  # syscall
        0x00404821,  # addu t1, v0, zero: expected 0
        0x24030065,  # addiu v1, zero, unknown 0x65
        0x0000000c,  # syscall: must remain unsupported
        0x240a0001,  # addiu t2, zero, 1: never executed
        0x00000008,  # jr zero
        0x00000000,  # delay slot
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_kernel_ram_handler_elf(path, invoke=False):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x180 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x80)
    struct.pack_into("<I", image, 52 + 20, 0x80)
    # Store a tiny guest code word at KSEG0 0x80076000, register that
    # direct RAM address for syscall 0x5B, and read back the kernel
    # table through KSEG0. Invoke it or clear the registration.
    instructions = (
        0x3c0a8000,  # lui t2, 0x8000 (kernel table alias)
        0x3c0b8007,  # lui t3, 0x8007
        0x356b6000,  # ori t3, t3, 0x6000
        0x3c0c03e0,  # lui t4, 0x03e0 (JR RA opcode)
        0x358c0008,  # ori t4, t4, 8
        0xad6c0000,  # sw t4, 0(t3), writing RAM JR $ra
        0x3c0c2402,  # lui t4, 0x2402 (ADDIU v0,zero,42 opcode)
        0x358c002a,  # ori t4, t4, 42
        0xad6c0004,  # sw t4, 4(t3), JR RA delay slot writes v0=42
        0x2404005b,  # addiu a0, zero, 0x5B
        0x01602821,  # addu a1, t3, zero: 0x80076000
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d48196c,  # lw t0, 0x196c(t2): registered pointer
        0x8d690000,  # lw t1, 0(t3): actual guest RAM instruction
    )
    end = (
        (
            0x2403005b,  # addiu v1, zero, 0x5B
            0x0000000c,  # syscall: execute the RAM callback
            0x00406821,  # addu t5, v0, zero: must observe 42
        ) if invoke else (
            0x2404005b,  # addiu a0, zero, 0x5B
            0x00002821,  # addu a1, zero, zero: unregister
            0x24030074,  # addiu v1, zero, SetSyscall
            0x0000000c,  # syscall
            0x8d4d196c,  # lw t5, 0x196c(t2): cleared table entry
        )
    )
    for i, opcode in enumerate(instructions + end + (0x00000008, 0x00000000)):
        struct.pack_into("<I", image, 0x108 + 4 * i, opcode)
    path.write_bytes(image)


def make_kernel_ram_probe_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    for offset, opcode in (
        (0x108, 0x3c048000),  # lui a0, 0x8000
        (0x10c, 0x8c820000),  # lw v0, 0(a0)
        (0x110, 0x00404021),  # addu t0, v0, zero
        (0x114, 0x00000008),  # jr zero
        (0x118, 0x00000000),  # nop
    ):
        struct.pack_into("<I", image, offset, opcode)
    path.write_bytes(image)


def make_spin_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # A branch to itself remains inside a single translated CFG. The
    # dispatcher count cannot interrupt this native loop.
    struct.pack_into("<I", image, 0x108, 0x1000ffff)  # beq zero,zero,-1
    struct.pack_into("<I", image, 0x10c, 0x00000000)  # delay-slot nop
    path.write_bytes(image)


def make_call_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # JAL 0x00100030; NOP; JR $zero; NOP. The callee returns with JR $ra.
    struct.pack_into("<I", image, 0x108, 0x0c04000c)
    struct.pack_into("<I", image, 0x10c, 0x00000000)
    struct.pack_into("<I", image, 0x110, 0x00000008)
    struct.pack_into("<I", image, 0x114, 0x00000000)
    struct.pack_into("<I", image, 0x130, 0x03e00008)
    struct.pack_into("<I", image, 0x134, 0x00000000)
    path.write_bytes(image)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_game_runner.py source_root build_dir runner_executable"
        )
    source = Path(sys.argv[1]).resolve()
    build = Path(sys.argv[2]).resolve()
    runner = Path(sys.argv[3]).resolve()
    # Multi-config CMake builds must select build/Release rather than a
    # stale build-root executable. Exercise the path selection on CI too.
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "run_game_driver", source / "tools" / "run_game.py"
    )
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    with tempfile.TemporaryDirectory(prefix="ee-multiconfig-lookup-") as tmp:
        fake_build = Path(tmp)
        (fake_build / "CMakeCache.txt").write_text(
            "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n", encoding="utf-8"
        )
        suffix = ".exe" if sys.platform == "win32" else ""
        old = fake_build / ("ee_recomp" + suffix)
        preferred = fake_build / "Release" / ("ee_recomp" + suffix)
        preferred.parent.mkdir()
        old.write_bytes(b"stale")
        preferred.write_bytes(b"current")
        assert driver.native_executable(fake_build, "ee_recomp") == preferred
        try:
            driver.native_executable(fake_build, "ee_recomp", "Debug")
        except ValueError:
            pass
        else:
            raise AssertionError("multi-config lookup used a stale root binary")


    help_result = subprocess.run(
        [str(runner), "--help"], capture_output=True, text=True
    )
    assert help_result.returncode == 0, help_result.stderr
    assert "--max-dispatches=N" in help_result.stdout
    # Requesting a Windows PDB on another host must fail rather than
    # pretending a useful symbols file was generated.
    if sys.platform != "win32":
        denied = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(source / "CMakeLists.txt"), "--pdb",
            ],
            capture_output=True, text=True, check=False,
        )
        assert denied.returncode != 0, denied
        assert "--pdb requires a Windows" in denied.stderr, denied.stderr
        link_denied = subprocess.run(
            [
                sys.executable, str(source / "tools" / "link_native_objects.py"),
                "missing.json", "missing-objects", "missing.so", "--pdb",
            ],
            capture_output=True, text=True, check=False,
        )
        assert link_denied.returncode != 0, link_denied
        assert "--pdb requires a Windows" in link_denied.stderr, link_denied.stderr

    assert "NOT guest instructions" in help_result.stdout

    with tempfile.TemporaryDirectory(prefix="ee-native-game-runner-") as tmp:
        root = Path(tmp)
        elf = root / "minimal-mips.elf"
        make_elf(elf)

        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--trace",
                "--kseg-ram",
                "--report", str(root / "first_boot.json"),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        # The guest JR $ra returns outside compiled code. The runner must
        # report an incomplete execution, not falsely declare a PS2 boot.
        assert result.returncode != 0, result.stdout + result.stderr
        assert "EE ELF entry 0x00100008" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "PT_LOAD 0x00100000" in result.stdout
        assert "EE stopped: dispatched=" in result.stdout
        assert "dispatch=1 pc=0x00100008" in result.stdout
        assert "guest left compiled code" in result.stderr or (
            "guest blocked" in result.stderr
        ), result.stdout + result.stderr
        report = json.loads((root / "first_boot.json").read_text(encoding="utf-8"))
        assert report["schema"] == "ee-game-run-report-v1", report
        assert report["snapshot_kind"] == "stopped", report
        assert report["entry_pc"] == 0x00100008, report
        assert report["dispatches"] >= 1, report
        assert report["history"][0]["from_pc"] == 0x00100008, report
        assert report["history"][-1]["dispatch"] == report["dispatches"], report
        assert len(report["gpr_lo"]) == 32, report
        assert (root / "out" / "ir" / "ee_functions.json").is_file()
        assert (root / "out" / "objects" / "objects.json").is_file()
        link_only = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--no-recompile", "--link-only",
            ],
            capture_output=True, text=True, check=False,
        )
        assert link_only.returncode == 0, link_only.stdout + link_only.stderr
        assert "native library ready:" in link_only.stdout, link_only.stdout
        assert "EE stopped:" not in link_only.stdout, link_only.stdout

        # A replay must reject old native code when the same ELF path
        # changes, even if its entry and translated function map do not.
        cache_path = root / "out" / "translation-cache.json"
        provenance = json.loads(cache_path.read_text(encoding="utf-8"))
        assert provenance["schema"] == "ee-translation-cache-v1", provenance
        cached_command = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf), "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--no-recompile", "--link-only",
        ]
        original_elf = elf.read_bytes()
        altered_elf = bytearray(original_elf)
        altered_elf[-1] ^= 1
        elf.write_bytes(altered_elf)
        changed_elf = subprocess.run(
            cached_command, capture_output=True, text=True, check=False,
        )
        assert changed_elf.returncode != 0, changed_elf
        assert "ELF changed" in changed_elf.stderr, changed_elf.stderr
        elf.write_bytes(original_elf)

        # An unchanged ELF still cannot reuse code from a different
        # recompiler build or a different LLVM object optimization mode.
        changed_compiler = dict(provenance)
        changed_compiler["inputs"] = dict(provenance["inputs"])
        changed_compiler["inputs"]["ee_recomp"] = "0" * 64
        cache_path.write_text(
            json.dumps(changed_compiler) + "\n", encoding="utf-8",
        )
        mismatched_compiler = subprocess.run(
            cached_command, capture_output=True, text=True, check=False,
        )
        assert mismatched_compiler.returncode != 0, mismatched_compiler
        assert "ee_recomp changed" in mismatched_compiler.stderr, (
            mismatched_compiler.stderr
        )
        cache_path.write_text(
            json.dumps(provenance) + "\n", encoding="utf-8",
        )
        wrong_optimization = subprocess.run(
            cached_command + ["--emit-opt=2"],
            capture_output=True, text=True, check=False,
        )
        assert wrong_optimization.returncode != 0, wrong_optimization
        assert "different --emit-opt" in wrong_optimization.stderr, (
            wrong_optimization.stderr
        )

        library_name = (
            "game-translated.dll" if sys.platform == "win32" else
            "libgame-translated.dylib" if sys.platform == "darwin" else
            "libgame-translated.so"
        )
        library = root / "out" / library_name
        original_library = library.read_bytes()
        altered_library = bytearray(original_library)
        altered_library[-1] ^= 1
        library.write_bytes(altered_library)
        changed_library = subprocess.run(
            cached_command, capture_output=True, text=True, check=False,
        )
        assert changed_library.returncode != 0, changed_library
        assert f"{library_name} changed" in changed_library.stderr, (
            changed_library.stderr
        )
        library.write_bytes(original_library)

        # Confirm a completely unchanged translation is still reusable.
        valid_cached = subprocess.run(
            cached_command, capture_output=True, text=True, check=False,
        )
        assert valid_cached.returncode == 0, (
            valid_cached.stdout + valid_cached.stderr
        )

        # Compare unoptimized and optimized native EE output for the same
        # guest instructions. Optimizing host code must not change EE state.
        optimized_report = root / "optimized.json"
        optimized = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "optimized"),
                "--max-dispatches=8",
                "--emit-opt=2",
                "--kseg-ram",
                "--report", str(optimized_report),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        optimized_state = json.loads(optimized_report.read_text(encoding="utf-8"))
        optimized_objects = json.loads(
            (root / "optimized" / "objects" / "objects.json").read_text(encoding="utf-8")
        )
        assert optimized_objects["optimization_level"] == 2, optimized_objects
        for field in ("entry_pc", "final_pc", "dispatches", "exit_reason",
                      "dispatch_status", "gpr_lo"):
            assert optimized_state[field] == report[field], (field, optimized_state, report)
        assert optimized.returncode == result.returncode, (
            optimized.stdout + optimized.stderr
        )


    with tempfile.TemporaryDirectory(prefix="ee-native-setupthread-") as tmp:
        root = Path(tmp)
        elf = root / "startup-mips.elf"
        make_elf(elf, startup=True)
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--hle-startup",
                "--trace",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "HLE SetupThread:" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "EE stopped: dispatched=2" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "guest left compiled code" in result.stderr, (
            result.stdout + result.stderr
        )

    with tempfile.TemporaryDirectory(prefix="ee-native-auto-expand-") as tmp:
        root = Path(tmp)
        elf = root / "caller-callee.elf"
        make_call_elf(elf)
        base = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf),
            "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--max-dispatches=8",
            "--trace",
        ]
        initial = subprocess.run(
            base,
            capture_output=True,
            text=True,
            check=False,
        )
        assert "EE stopped: dispatched=" in initial.stdout, (
            initial.stdout + initial.stderr
        )
        assert "auto-expand: compiling" not in initial.stdout, initial.stdout
        metadata_path = root / "out" / "ir" / "ee_functions.json"
        assert json.loads(metadata_path.read_text(encoding="utf-8"))["function_count"] == 1

        resumed = subprocess.run(
            base + ["--no-recompile", "--auto-expand=1"],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "auto-expand: compiling reachable PC 0x00100030" in resumed.stdout, (
            resumed.stdout + resumed.stderr
        )
        assert "EE stopped: dispatched=" in resumed.stdout, (
            resumed.stdout + resumed.stderr
        )
        assert "ee_recomp: loading" in resumed.stdout, resumed.stdout
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        assert metadata["function_count"] == 2, metadata

    for invalid_set in (False, True):
        with tempfile.TemporaryDirectory(prefix="ee-native-osd-config-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-osd-config.elf"
            make_osd_config_elf(elf, invalid_set=invalid_set)
            report_path = root / "osd-config.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16",
                "--trace",
                "--report", str(report_path),
            ]

            # The kernel must never be silently enabled without HLE.
            if not invalid_set:
                without_hle = subprocess.run(
                    command, capture_output=True, text=True, check=False,
                )
                assert without_hle.returncode != 0, (
                    without_hle.stdout + without_hle.stderr
                )
                refused = json.loads(report_path.read_text(encoding="utf-8"))
                assert refused["cop0_epc"] == 0x00100014, refused
                assert "HLE GetOsdConfigParam:" not in without_hle.stdout
                command.append("--no-recompile")

            with_hle = subprocess.run(
                command + ["--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert with_hle.returncode != 0, with_hle.stdout + with_hle.stderr
            assert "HLE GetOsdConfigParam: pointer=0x00100200 value=0x00000000 result=0" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE SetOsdConfigParam: pointer=0x00100210 value=0x12345678 result=0" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE GetOsdConfigParam: pointer=0x00100220 value=0x12345678 result=0" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            denied_service = "Set" if invalid_set else "Get"
            denied_address = 0x00100221 if invalid_set else 0x7ffffffc
            assert (
                f"{denied_service}OsdConfigParam HLE refused unbacked or "
                f"unaligned pointer 0x{denied_address:08x}"
            ) in with_hle.stderr, with_hle.stdout + with_hle.stderr

            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["cop0_epc"] == 0x00100058, state
            registers = state["gpr_lo"]
            assert registers[8] == 0, state           # initially zero
            assert registers[9] == 0, state           # Get returns zero
            assert registers[10] == 0, state          # Set returns zero
            assert registers[11] == 0x12345678, state # Set persisted
            assert registers[12] == 0, state          # second Get result
            assert registers[13] == 0, state          # no fallthrough
            assert state["exit_name"] == "EXCEPTION", state

    with tempfile.TemporaryDirectory(prefix="ee-native-thread-priority-") as tmp:
        root = Path(tmp)
        elf = root / "kernel-thread-priority.elf"
        make_thread_priority_elf(elf)
        report_path = root / "thread-priority.json"
        command = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf),
            "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--max-dispatches=32",
            "--trace",
            "--report", str(report_path),
        ]

        without_hle = subprocess.run(
            command, capture_output=True, text=True, check=False,
        )
        assert without_hle.returncode != 0, (
            without_hle.stdout + without_hle.stderr
        )
        refused = json.loads(report_path.read_text(encoding="utf-8"))
        assert refused["cop0_epc"] == 0x0010000c, refused
        assert "HLE ChangeThreadPriority:" not in without_hle.stdout

        with_hle = subprocess.run(
            command + ["--no-recompile", "--hle-startup"],
            capture_output=True, text=True, check=False,
        )
        assert "HLE ChangeThreadPriority: id=1 old=0 new=1" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        assert "HLE ChangeThreadPriority: id=1 old=1 new=5" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        assert "HLE iChangeThreadPriority: id=2 old=64 new=2" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        assert "HLE ChangeThreadPriority: id=2 old=2 new=7" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        assert "HLE ChangeThreadPriority: id=99 not found, result=-1" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        state = json.loads(report_path.read_text(encoding="utf-8"))
        registers = state["gpr_lo"]
        assert registers[8] == 1, state   # GetThreadId before changes
        assert registers[9] == 0, state   # old main priority
        assert registers[10] == 1, state  # old priority using ID alias 0
        assert registers[11] == 2, state  # dormant CreateThread result
        assert registers[12] == 64, state # old dormant priority
        assert registers[13] == 2, state  # masked priority persisted
        assert registers[14] == 0xFFFFFFFFFFFFFFFF, state  # invalid thread
        assert registers[15] == 1, state  # GetThreadId remains main
        assert state["final_pc"] != 0x001000f0, state # no child ran

    with tempfile.TemporaryDirectory(prefix="ee-native-get-thread-id-") as tmp:
        root = Path(tmp)
        elf = root / "kernel-get-thread-id.elf"
        make_get_thread_id_elf(elf)
        report_path = root / "get-thread-id.json"
        command = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf),
            "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--max-dispatches=16",
            "--trace",
            "--report", str(report_path),
        ]

        without_hle = subprocess.run(
            command, capture_output=True, text=True, check=False,
        )
        assert without_hle.returncode != 0, (
            without_hle.stdout + without_hle.stderr
        )
        refused = json.loads(report_path.read_text(encoding="utf-8"))
        assert refused["cop0_epc"] == 0x0010000c, refused
        assert "HLE GetThreadId:" not in without_hle.stdout, (
            without_hle.stdout + without_hle.stderr
        )

        with_hle = subprocess.run(
            command + ["--no-recompile", "--hle-startup"],
            capture_output=True, text=True, check=False,
        )
        assert with_hle.stdout.count("HLE GetThreadId: id=1") == 2, (
            with_hle.stdout + with_hle.stderr
        )
        assert "HLE CreateThread: id=2" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        state = json.loads(report_path.read_text(encoding="utf-8"))
        assert state["gpr_lo"][8] == 1, state  # running main thread
        assert state["gpr_lo"][9] == 2, state  # new dormant thread
        assert state["gpr_lo"][10] == 1, state # main remains current
        assert state["gpr_lo"][2] == 1, state  # GetThreadId return value
        assert state["gpr_lo"][4] == 0x00100200, state # ignores a0
        assert state["final_pc"] != 0x001000f0, state

    with tempfile.TemporaryDirectory(prefix="ee-native-create-thread-") as tmp:
        root = Path(tmp)
        elf = root / "kernel-create-thread.elf"
        make_create_thread_elf(elf)
        report_path = root / "create-thread.json"
        command = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf),
            "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--max-dispatches=16",
            "--trace",
            "--report", str(report_path),
        ]

        without_hle = subprocess.run(
            command, capture_output=True, text=True, check=False,
        )
        assert without_hle.returncode != 0, (
            without_hle.stdout + without_hle.stderr
        )
        refused = json.loads(report_path.read_text(encoding="utf-8"))
        assert refused["cop0_epc"] == 0x00100014, refused
        assert "HLE CreateThread:" not in without_hle.stdout, (
            without_hle.stdout + without_hle.stderr
        )

        with_hle = subprocess.run(
            command + ["--no-recompile", "--hle-startup"],
            capture_output=True, text=True, check=False,
        )
        assert with_hle.returncode != 0, (
            with_hle.stdout + with_hle.stderr
        )
        assert ("HLE CreateThread: id=2 entry=0x001000f0 "
                "stack=0x00102000 size=0x00000800 gp=0x0010f000 "
                "priority=64 attr=0x0000abcd option=0xaabbccdd "
                "parameters=0x00100200 dormant") in with_hle.stdout, (
                    with_hle.stdout + with_hle.stderr
                )
        assert ("HLE CreateThread: id=3 entry=0x001000f0 "
                "stack=0x00102000 size=0x00000800 gp=0x0010f000 "
                "priority=64 attr=0x0000abcd option=0xaabbccdd "
                "parameters=0x00100200 dormant") in with_hle.stdout, (
                    with_hle.stdout + with_hle.stderr
                )
        assert ("CreateThread HLE refused unbacked or unaligned "
                "parameter block 0x00100201") in with_hle.stderr, (
                    with_hle.stdout + with_hle.stderr
                )
        state = json.loads(report_path.read_text(encoding="utf-8"))
        assert state["cop0_epc"] == 0x0010003c, state
        assert state["gpr_lo"][8] == 2, state
        assert state["gpr_lo"][9] == 3, state
        assert state["gpr_lo"][10] == 0, state
        assert state["gpr_lo"][2] == 3, state
        assert state["final_pc"] != 0x001000f0, state

    with tempfile.TemporaryDirectory(prefix="ee-native-semaphore-") as tmp:
        root = Path(tmp)
        elf = root / "semaphore-mips.elf"
        make_semaphore_elf(elf)
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--hle-startup",
                "--trace",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "HLE CreateSema: id=1 init=1 max=2" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "EE stopped: dispatched=2" in result.stdout, (
            result.stdout + result.stderr
        )

    with tempfile.TemporaryDirectory(prefix="ee-native-flushcache-") as tmp:
        root = Path(tmp)
        elf = root / "flush-cache-mips.elf"
        make_flush_cache_elf(elf)
        report = root / "flush-cache.json"
        command = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf),
            "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--max-dispatches=8",
            "--report", str(report),
            "--trace",
        ]
        without_hle = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
        )
        assert without_hle.returncode != 0, without_hle.stdout + without_hle.stderr
        initial = json.loads(report.read_text(encoding="utf-8"))
        assert initial["cop0_epc"] == 0x00100010, initial
        assert initial["gpr_lo"][2] == 123, initial
        assert "HLE FlushCache" not in without_hle.stdout, without_hle.stdout

        with_hle = subprocess.run(
            command + ["--no-recompile", "--hle-startup"],
            capture_output=True,
            text=True,
            check=False,
        )
        assert with_hle.returncode != 0, with_hle.stdout + with_hle.stderr
        assert "HLE FlushCache: mode=0x00000000 result=0" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        assert "HLE iFlushCache: mode=0x00000000 result=0" in with_hle.stdout, (
            with_hle.stdout + with_hle.stderr
        )
        stopped = json.loads(report.read_text(encoding="utf-8"))
        assert stopped["cop0_epc"] == 0x0010002c, stopped
        assert stopped["gpr_lo"][8] == 0, stopped  # first service return
        assert stopped["gpr_lo"][9] == 0, stopped  # second service return
        assert stopped["gpr_lo"][10] == 0, stopped  # unknown call never returns
        assert stopped["gpr_lo"][2] == 0, stopped
        assert stopped["gpr_lo"][3] == 0x65, stopped
        assert "guest blocked by an unsupported operation" in with_hle.stderr, (
            with_hle.stdout + with_hle.stderr
        )

    with tempfile.TemporaryDirectory(prefix="ee-native-syscall-hook-") as tmp:
        root = Path(tmp)
        elf = root / "set-syscall-mips.elf"
        make_set_syscall_elf(elf)
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--hle-startup",
                "--trace",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "HLE SetSyscall: number=0x83 handler=0x00100030" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "EE stopped: dispatched=2" in result.stdout, (
            result.stdout + result.stderr
        )

    for status, expected_target in (
        (2, 0x001000c0),  # EXL: ordinary EPC
        (4, 0x00100080),  # ERL: ErrorEPC takes precedence
    ):
        with tempfile.TemporaryDirectory(prefix="ee-native-eret-") as tmp:
            root = Path(tmp)
            elf = root / "eret.elf"
            make_eret_target_elf(elf, status)
            report_path = root / "eret.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=16",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True, text=True, check=False,
            )
            assert result.returncode != 0, result.stdout + result.stderr
            assert (
                f"dispatch=1 pc=0x00100008 -> 0x{expected_target:08x} "
                "reason=DISPATCH(3) status=1"
            ) in result.stdout, result.stdout + result.stderr
            report = json.loads(report_path.read_text(encoding="utf-8"))
            assert report["dispatches"] == 2, report
            assert report["final_pc"] == 0, report
            assert report["gpr_lo"][11] == 0, report
            assert report["gpr_lo"][12] == 0x00100080, report
            assert report["gpr_lo"][13] == 0x001000c0, report
            assert report["gpr_lo"][15] == 0, report

    with tempfile.TemporaryDirectory(prefix="ee-native-overlapping-entry-") as tmp:
        root = Path(tmp)
        elf = root / "shared-successor.elf"
        make_overlapping_entry_elf(elf)
        report_path = root / "shared-successor.json"
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=64",
                "--auto-expand=2",
                "--hle-startup",
                "--trace",
                "--report", str(report_path),
            ],
            capture_output=True, text=True, check=False,
        )
        assert result.returncode != 0, result.stdout + result.stderr
        assert "auto-expand: compiling reachable PC 0x00100050" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "auto-expand: compiling reachable PC 0x00100054" in result.stdout, (
            result.stdout + result.stderr
        )
        assert ("auto-expand: preserving existing ownership of 1 "
                "overlapping guest block PC(s)") in result.stdout, (
            result.stdout + result.stderr
        )
        assert result.stdout.count("HLE FlushCache: mode=0x00000000 result=0") >= 2, (
            result.stdout + result.stderr
        )

        metadata = json.loads(
            (root / "out" / "ir" / "ee_functions.json").read_text(encoding="utf-8")
        )
        owners = [
            int(pc, 0) for function in metadata["functions"]
            for pc in function["block_pcs"]
        ]
        assert len(owners) == len(set(owners)), metadata
        assert owners.count(0x00100050) == 1, metadata
        assert owners.count(0x00100054) == 1, metadata
        assert owners.count(0x00100058) == 1, metadata
        state = json.loads(report_path.read_text(encoding="utf-8"))
        assert state["dispatches"] >= 7, state
        assert state["final_pc"] == 0, state
        assert state["gpr_lo"][9] == 0, state  # second real syscall return
        assert state["gpr_lo"][16] == 0, state  # first real syscall return

    with tempfile.TemporaryDirectory(prefix="ee-native-interrupt-masks-") as tmp:
        root = Path(tmp)
        elf = root / "kernel-interrupt-masks.elf"
        make_interrupt_mask_elf(elf)
        report_path = root / "masks.json"
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=64",
                "--hle-startup",
                "--trace",
                "--report", str(report_path),
            ],
            capture_output=True, text=True, check=False,
        )
        assert result.returncode != 0, result.stdout + result.stderr
        for name in (
            "EnableIntc", "DisableIntc", "EnableDmac", "DisableDmac",
        ):
            assert f"HLE {name}" in result.stdout, result.stdout + result.stderr
        registers = json.loads(report_path.read_text(encoding="utf-8"))["gpr_lo"]
        assert registers[8:16] == [1, 0, 1, 0, 1, 1, 1, 0], registers
        assert registers[16:22] == [1, 0, 1, 1, 1, 1], registers
        assert registers[22] == (1 << 11), registers  # INTC_MASK
        assert registers[23] == (1 << 18), registers  # DMAC D_STAT
        assert "interrupt mask HLE refused" not in result.stderr, result.stderr

    with tempfile.TemporaryDirectory(prefix="ee-native-interrupt-handlers-") as tmp:
        root = Path(tmp)
        elf = root / "kernel-interrupt-handlers.elf"
        make_interrupt_handlers_elf(elf)
        report_path = root / "handlers.json"
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=32",
                "--hle-startup",
                "--trace",
                "--report", str(report_path),
            ],
            capture_output=True, text=True, check=False,
        )
        assert result.returncode != 0, result.stdout + result.stderr
        assert "EE stopped: dispatched=" in result.stdout, (
            result.stdout + result.stderr
        )
        for item in (
            "HLE AddIntcHandler: id=1 cause=11 handler=0x00100090 "
            "next=0xffffffff arg=0x00000066 gp=0x12340000",
            "HLE AddIntcHandler: id=2 cause=11 handler=0x001000a0 "
            "next=0x00000000 arg=0x00000077 gp=0x12340000",
            "HLE AddDmacHandler: id=3 cause=2 handler=0x001000b0 "
            "next=0xffffffff arg=0x00000088 gp=0x12340000",
            "HLE RemoveIntcHandler: cause=11 id=2 remaining=1",
            "HLE RemoveIntcHandler: cause=11 id=99 remaining=1",
            "HLE RemoveIntcHandler: cause=11 id=1 remaining=0",
            "HLE RemoveDmacHandler: cause=2 id=3 remaining=0",
        ):
            assert item in result.stdout, (item, result.stdout + result.stderr)
        registers = json.loads(report_path.read_text(encoding="utf-8"))["gpr_lo"]
        assert registers[8:14] == [1, 2, 3, 1, 0, 0], registers
        assert registers[16] == 1, registers

    with tempfile.TemporaryDirectory(prefix="ee-native-syscall-out-of-range-") as tmp:
        root = Path(tmp)
        elf = root / "out-of-range-syscalls.elf"
        make_out_of_range_set_syscall_elf(elf)
        report_path = root / "out-of-range.json"
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16",
                "--hle-startup",
                "--kseg-ram",
                "--trace",
                "--report", str(report_path),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "HLE SetSyscall: number=0x83 handler=0x00100050" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "HLE SetSyscall: unsupported number=0x0000012c handler=0x80076488 (ignored)" in result.stderr, (
            result.stdout + result.stderr
        )
        assert "HLE SetSyscall: unsupported number=0xffffffff handler=0x80076488 (ignored)" in result.stderr, (
            result.stdout + result.stderr
        )
        assert "EE stopped: dispatched=" in result.stdout, (
            result.stdout + result.stderr
        )
        report = json.loads(report_path.read_text(encoding="utf-8"))
        registers = report["gpr_lo"]
        assert registers[8] == 0x00100050, report
        assert registers[9] == 0, report  # 0x12C returns zero
        assert registers[11] == 0x00100050, report
        assert registers[12] == 0, report  # past table untouched
        assert registers[13] == 0, report  # huge number returns zero
        assert registers[14] == 0x00100050, report
        assert registers[15] == 0, report  # still untouched
        assert registers[2] == 0, report

    with tempfile.TemporaryDirectory(prefix="ee-native-syscall-table-") as tmp:
        root = Path(tmp)
        elf = root / "guest-syscall-table.elf"
        make_guest_syscall_table_elf(elf)
        report_path = root / "kernel-table.json"
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=24",
                "--hle-startup",
                "--kseg-ram",
                "--trace",
                "--report", str(report_path),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "HLE SetSyscall: number=0x83 handler=0x00100050" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "HLE SetSyscall: number=0x5a handler=0x00100030" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "HLE SetSyscall: number=0x83 handler=0x00100030" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "EE stopped: dispatched=" in result.stdout, (
            result.stdout + result.stderr
        )
        report = json.loads(report_path.read_text(encoding="utf-8"))
        registers = report["gpr_lo"]
        assert registers[8] == 0, report  # uninitialized table starts empty
        assert registers[9] == 0x00100050, report  # 0x83 first installation
        assert registers[11] == 0x00100050, report  # physical alias
        assert registers[12] == 0x00100030, report  # 0x5A at 0x1968
        assert registers[13] == 0x00100050, report  # 0x83 unchanged by 0x5A
        assert registers[14] == 0x00100030, report  # 0x83 replacement
        assert registers[15] == 0, report  # untouched 0x84 at 0x1A10

    with tempfile.TemporaryDirectory(prefix="ee-native-kernel-ram-handler-") as tmp:
        root = Path(tmp)
        for invoke in (False, True):
            elf = root / ("invoke-ram-handler.elf" if invoke else "clear-ram-handler.elf")
            make_kernel_ram_handler_elf(elf, invoke=invoke)
            report_path = root / "ram-handler.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / ("invoke" if invoke else "clear")),
                    "--max-dispatches=16",
                    "--hle-startup",
                    "--kseg-ram",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "HLE SetSyscall: number=0x5b handler=0x80076000" in result.stdout, (
                result.stdout + result.stderr
            )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            # MIPS LW sign-extends KSEG0 pointers into 64-bit GPRs.
            assert report["gpr_lo"][8] & 0xffffffff == 0x80076000, report
            assert report["gpr_lo"][9] == 0x03e00008, report
            if invoke:
                assert "HLE guest RAM syscall returned: entry=0x80076000" in result.stdout, (
                    result.stdout + result.stderr
                )
                assert report["gpr_lo"][13] == 42, report
                events = report["syscall_history"]
                assert len(events) == 1 and events[0]["service"] == 0x5b, report
                assert events[0]["handler"] == 0x80076000, report
                assert events[0]["v0"] == 42 and events[0]["returned"], report
            else:
                assert "HLE SetSyscall: number=0x5b handler=0x00000000" in result.stdout, (
                    result.stdout + result.stderr
                )
                assert report["gpr_lo"][13] == 0, report

    with tempfile.TemporaryDirectory(prefix="ee-native-registered-callback-") as tmp:
        root = Path(tmp)
        elf = root / "guest-callback-mips.elf"
        make_registered_callback_elf(elf)
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16",
                "--auto-expand=1",
                "--hle-startup",
                "--trace",
                "--report", str(root / "callback.json"),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "auto-expand: compiling reachable PC 0x00100050" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "HLE guest syscall: number=0x83 handler=0x00100050" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "HLE guest syscall returned: resume=0x00100028 v0=0x0000000c" in result.stdout, (
            result.stdout + result.stderr
        )
        report = json.loads((root / "callback.json").read_text(encoding="utf-8"))
        assert report["gpr_lo"][8] == 12, report
        assert report["gpr_lo"][31] == 0, report
        assert report["dispatches"] >= 4, report
        events = report["syscall_history"]
        assert len(events) == 1, report
        assert events[0]["service"] == 0x83, events
        assert events[0]["handler"] == 0x00100050, events
        assert events[0]["resume_pc"] == 0x00100028, events
        assert events[0]["a0"] == 7, events
        assert events[0]["v0"] == 12, events
        assert events[0]["returned"] is True, events

    with tempfile.TemporaryDirectory(prefix="ee-native-timeout-snapshot-") as tmp:
        root = Path(tmp)
        elf = root / "loop-mips.elf"
        make_spin_elf(elf)
        report_path = root / "timeout.json"
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--timeout-seconds=1",
                "--report", str(report_path),
                "--trace",
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=40,
        )
        assert result.returncode == 124, result.stdout + result.stderr
        snapshot = json.loads(report_path.read_text(encoding="utf-8"))
        assert snapshot["snapshot_kind"] == "in_progress", snapshot
        assert snapshot["dispatches"] == 0, snapshot
        assert snapshot["syscall_history"] == [], snapshot
        assert snapshot["final_pc"] == 0x00100008, snapshot
        assert "last saved snapshot" in result.stderr, result.stderr

    with tempfile.TemporaryDirectory(prefix="ee-kernel-snapshot-") as tmp:
        root = Path(tmp)
        elf = root / "kernel-probe.elf"
        make_kernel_ram_probe_elf(elf)
        kernel = root / "kernel-ram.bin"
        kernel.write_bytes(struct.pack("<I", 0x12345678) + bytes(0x80000 - 4))
        archive_impostor = root / "wrong-snapshot.bin"
        archive_impostor.write_bytes(b"PK\x03\x04" + bytes(0x80000 - 4))
        report = root / "kernel-probe.json"
        cmd = [
            sys.executable, str(source / "tools" / "run_game.py"),
            str(elf),
            "--build-dir", str(build),
            "--work-dir", str(root / "out"),
            "--max-dispatches=8",
            "--kseg-ram",
            "--report", str(report),
        ]
        first = subprocess.run(cmd, capture_output=True, text=True, check=False)
        assert first.returncode != 0, first.stdout + first.stderr
        before = json.loads(report.read_text(encoding="utf-8"))
        assert before["gpr_lo"][8] == 0, before

        invalid = subprocess.run(
            cmd + ["--no-recompile", "--kernel-ram", str(archive_impostor)],
            capture_output=True, text=True, check=False,
        )
        assert invalid.returncode != 0, invalid.stdout + invalid.stderr
        assert "ZIP/savestate bytes" in invalid.stderr, invalid.stderr

        second = subprocess.run(
            cmd + ["--no-recompile", "--kernel-ram", str(kernel)],
            capture_output=True, text=True, check=False,
        )
        assert second.returncode != 0, second.stdout + second.stderr
        assert "EE kernel RAM snapshot: 524288 bytes" in second.stdout, (
            second.stdout + second.stderr
        )
        after = json.loads(report.read_text(encoding="utf-8"))
        assert after["gpr_lo"][8] == 0x12345678, after

    print("game_runner end-to-end ELF/native smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
