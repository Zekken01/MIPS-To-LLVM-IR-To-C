#!/usr/bin/env python3
"""Synthetic ELF32-to-native-runner integration test.

Runs a real MIPS ELF entry through the full emitter, object linker and
bounded native dispatcher. This is not a claim of PS2 BIOS bootability.
"""
from pathlib import Path
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

    help_result = subprocess.run(
        [str(runner), "--help"], capture_output=True, text=True
    )
    assert help_result.returncode == 0, help_result.stderr
    assert "--max-dispatches=N" in help_result.stdout
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
        assert (root / "out" / "ir" / "ee_functions.json").is_file()
        assert (root / "out" / "objects" / "objects.json").is_file()

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
        result = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--auto-expand=1",
                "--trace",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert "auto-expand: compiling reachable PC 0x00100030" in result.stdout, (
            result.stdout + result.stderr
        )
        assert "EE stopped: dispatched=" in result.stdout, (
            result.stdout + result.stderr
        )
        metadata = __import__("json").loads(
            (root / "out" / "ir" / "ee_functions.json").read_text(encoding="utf-8")
        )
        assert metadata["function_count"] == 2, metadata

    print("game_runner end-to-end ELF/native smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
