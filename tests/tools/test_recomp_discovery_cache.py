#!/usr/bin/env python3
"""Synthetic-ELF regression for fast single-function LLVM IR generation.

The game ELF is not used. Cached discovery must emit exactly the same IR
and block-entry metadata as independent full ELF discovery.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def make_elf(path, change=False):
    image = bytearray(0x200)
    image[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into(
        "<HHIIIIIHHHHHH", image, 16,
        2, 8, 1, 0x00100008, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<IIIIIIII", image, 52,
        1, 0x100, 0x00100000, 0x00100000, 0x100, 0x100, 5, 16,
    )
    instructions = (
        (0x108, 0x0c040010),  # JAL 0x00100040
        (0x10c, 0x00000000),
        (0x110, 0x03e00008),  # JR RA
        (0x114, 0x00000000),
        (0x140, 0x24020007 if not change else 0x24020008),
        (0x144, 0x03e00008),
        (0x148, 0x00000000),
    )
    for offset, word in instructions:
        struct.pack_into("<I", image, offset, word)
    path.write_bytes(image)


def compile_ir(exe, elf, output, function, cache=None):
    command = [exe, str(elf), str(output), f"--function=0x{function:08x}"]
    if cache is not None:
        command.append(f"--discovery-cache={cache}")
    return subprocess.run(command, capture_output=True, text=True)


def test_discovery_cache(ee_recomp):
    with tempfile.TemporaryDirectory(prefix="ee-discovery-cache-") as tmp:
        root = Path(tmp)
        elf = root / "synthetic.elf"
        make_elf(elf)
        full = root / "full"
        cache = full / "ee_discovery_cache.bin"
        baseline = compile_ir(ee_recomp, elf, full, 0x00100040)
        assert baseline.returncode == 0, baseline.stdout + baseline.stderr
        assert cache.is_file() and cache.stat().st_size > 32, cache

        reused = root / "reused"
        fast = compile_ir(ee_recomp, elf, reused, 0x00100040, cache)
        assert fast.returncode == 0, fast.stdout + fast.stderr
        assert "discovery cache:" in fast.stdout, fast.stdout

        first_meta = json.loads((full / "ee_functions.json").read_text())
        second_meta = json.loads((reused / "ee_functions.json").read_text())
        assert len(first_meta["functions"]) == 1
        assert len(second_meta["functions"]) == 1
        first_fn = first_meta["functions"][0]
        second_fn = second_meta["functions"][0]
        assert first_fn["block_pcs"] == second_fn["block_pcs"]
        assert first_fn["insn_count"] == second_fn["insn_count"]
        assert first_fn["supported_count"] == second_fn["supported_count"]
        assert first_fn["host_symbol"] == second_fn["host_symbol"]
        assert Path(first_fn["ir_path"]).read_bytes() == Path(second_fn["ir_path"]).read_bytes()

        # Compare two independent target entries, including one that was
        # not in the static function entry set, against the cached result.
        dynamic_full = root / "dynamic-full"
        dynamic_fast = root / "dynamic-fast"
        result = compile_ir(ee_recomp, elf, dynamic_full, 0x00100044)
        assert result.returncode == 0, result.stdout + result.stderr
        result = compile_ir(ee_recomp, elf, dynamic_fast, 0x00100044, cache)
        assert result.returncode == 0, result.stdout + result.stderr
        a = json.loads((dynamic_full / "ee_functions.json").read_text())
        b = json.loads((dynamic_fast / "ee_functions.json").read_text())
        assert a["functions"][0]["block_pcs"] == b["functions"][0]["block_pcs"]
        assert (Path(a["functions"][0]["ir_path"]).read_bytes()
                == Path(b["functions"][0]["ir_path"]).read_bytes())

        # Never silently reuse discovery from a different executable.
        changed = root / "changed.elf"
        make_elf(changed, change=True)
        denied = compile_ir(ee_recomp, changed, root / "changed", 0x00100040, cache)
        assert denied.returncode != 0, denied.stdout
        assert "stale or invalid" in denied.stderr, denied.stderr

        # Truncation cannot produce partial functions or a false cache hit.
        cache.write_bytes(cache.read_bytes()[:32])
        denied = compile_ir(ee_recomp, elf, root / "truncated", 0x00100040, cache)
        assert denied.returncode != 0, denied.stdout
        assert "stale or invalid" in denied.stderr, denied.stderr
    print("PASS: fast EE discovery produces identical IR and rejects invalid caches")


if __name__ == "__main__":
    test_discovery_cache(sys.argv[1])
