#!/usr/bin/env python3
"""Exercise the standalone IOP firmware runner without shipping BIOS data."""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROM_BYTES = 4 * 1024 * 1024


def make_rom(instructions: list[int], include_romdir: bool = True) -> bytes:
    data = bytearray(b"\xff" * ROM_BYTES)
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", data, index * 4, opcode)

    if include_romdir:
        def entry(offset: int, name: str, size: int) -> None:
            data[offset:offset + 10] = name.encode("ascii").ljust(10, b"\0")
            struct.pack_into("<HI", data, offset + 10, 0, size)

        entry(0x1000, "RESET", 4096)
        entry(0x1010, "ROMDIR", 64)
        entry(0x1020, "IOPBOOT", 256)
        data[0x1030:0x1040] = bytes(16)

    return bytes(data)


def run_case(
    executable: Path,
    base: Path,
    name: str,
    instructions: list[int],
    *,
    budget: int = 32,
    code: int = 3,
    include_romdir: bool = True,
) -> tuple[dict, subprocess.CompletedProcess[str]]:
    image = base / f"{name}.bin"
    report = base / f"{name}.json"
    image.write_bytes(make_rom(instructions, include_romdir))
    command = [
        str(executable),
        str(image),
        f"--instructions={budget}",
        "--trace-tail=8",
        f"--report={report}",
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    assert report.is_file(), result.stdout + result.stderr
    data = json.loads(report.read_text(encoding="utf-8"))
    assert data["format"] == "iop-rom0-diagnostics-v1", data
    assert data["rom0_bytes"] == ROM_BYTES, data
    assert data["reset_pc"] == 0xBFC00000, data
    assert data["romdir_has_iopboot"] is include_romdir, data
    assert data["cop0_status"] == 0x10900000, data
    assert data["cop0_prid"] == 0x1F, data
    assert len(data["gpr"]) == 32, data
    assert len(data["trace_tail"]) <= 8, data
    return data, result


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_iop_rom0_runner.py <binary>")

    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="iop-rom0-test-") as directory:
        root = Path(directory)

        normal, output = run_case(
            executable, root, "guest-breakpoint",
            [
                0x3C081F80,  # lui t0,0x1f80
                0x24091234,  # addiu t1,zero,0x1234
                0xAD090000,  # sw t1,0(t0), IOP scratchpad
                0x8D0A0000,  # lw t2,0(t0)
                0x0000000D,  # breakpoint
            ],
        )
        assert normal["stop"] == "BREAKPOINT", normal
        assert normal["executed"] == 4, normal
        assert normal["pc"] == 0xBFC00010, normal
        assert normal["gpr"][10] == 0x1234, normal
        assert normal["memory_fault"] is None, normal
        assert "IOPBOOT=present" in output.stdout, output.stdout
        assert "IOP trace: pc=0xbfc00010" in output.stdout, output.stdout

        console, output = run_case(
            executable, root, "iop-debug-console",
            [
                0x3C08BF80,  # lui t0,0xbf80 (IOP KSEG1)
                0x35082080,  # ori t0,t0,0x2080
                0x24090041,  # addiu t1,zero,'A'
                0xA1090000,  # sb t1,0(t0)
                0x0000000D,  # breakpoint
            ],
        )
        assert console["stop"] == "BREAKPOINT", console
        assert console["executed"] == 4, console
        assert console["console_output_hex"] == "41", console
        assert console["console_truncated"] is False, console
        assert "IOP console: 1 bytes" in output.stdout, output.stdout
        assert "A\n" in output.stdout, output.stdout

        mmio_read, output = run_case(
            executable, root, "unimplemented-mmio-read",
            [
                0x3C081F80,  # lui t0,0x1f80
                0x8D091100,  # lw t1,0x1100(t0) (unsupported timer)
                0x0000000D,
            ],
        )
        assert mmio_read["stop"] == "MEMORY_FAULT", mmio_read
        assert mmio_read["executed"] == 1, mmio_read
        assert mmio_read["pc"] == 0xBFC00004, mmio_read
        assert mmio_read["opcode"] == 0x8D091100, mmio_read
        assert mmio_read["memory_fault"] == {
            "address": 0x1F801100, "width": 4, "write": False
        }, mmio_read
        assert "guest_address=0x1f801100" in output.stdout, output.stdout

        mmio_write, _ = run_case(
            executable, root, "unimplemented-mmio-write",
            [
                0x3C081F80,
                0xAD091100,  # sw t1,0x1100(t0) (unsupported timer)
                0x0000000D,
            ],
        )
        assert mmio_write["memory_fault"] == {
            "address": 0x1F801100, "width": 4, "write": True
        }, mmio_write

        loop, output = run_case(
            executable, root, "bounded-firmware-loop",
            [
                0x1000FFFF,  # beq zero,zero,-1
                0x00000000,  # nop, delay slot
            ],
            budget=25,
            code=4,
            include_romdir=False,
        )
        assert loop["stop"] == "BUDGET_EXHAUSTED", loop
        assert loop["executed"] == 25, loop
        assert loop["memory_fault"] is None, loop
        assert "IOPBOOT=not found" in output.stdout, output.stdout

        tiny = root / "invalid.bin"
        tiny.write_bytes(b"\x00\x01\x02")
        rejected = subprocess.run(
            [str(executable), str(tiny)],
            capture_output=True,
            text=True,
        )
        assert rejected.returncode == 2, rejected.stdout + rejected.stderr
        assert "exactly 4194304 bytes" in rejected.stderr, rejected.stderr
        assert not (root / "invalid.json").exists()

        bad_args = subprocess.run(
            [str(executable), str(tiny), "--trace-tail=999"],
            capture_output=True, text=True,
        )
        assert bad_args.returncode == 2, bad_args.stdout + bad_args.stderr

    print("IOP standalone ROM0 diagnostic runner: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
