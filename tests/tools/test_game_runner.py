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
