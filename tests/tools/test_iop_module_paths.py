#!/usr/bin/env python3
"""Synthetic ELF fixtures for local SDBZ IOP path-reference discovery."""
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parents[2]
TOOL = SOURCE_ROOT / "tools" / "analyze_iop_module_paths.py"
spec = importlib.util.spec_from_file_location("iop_module_paths", TOOL)
analyzer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = analyzer
spec.loader.exec_module(analyzer)


def make_elf(base=0x00100000, size=0x180):
    raw = bytearray(0x100 + size)
    raw[0:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", raw, 16, 2, 8, 1)
    struct.pack_into("<I", raw, 28, 52)
    struct.pack_into("<HH", raw, 40, 52, 32)
    struct.pack_into("<H", raw, 44, 1)
    struct.pack_into("<IIIIIIII", raw, 52,
                     1, 0x100, base, base, size, size, 5, 4)
    return raw


def test_discovery():
    raw = make_elf()
    instructions = (
        0x3c080010,  # lui t0,0x0010
        0x25040080,  # addiu a0,t0,0x80 = first path pointer
        0x3c090010,  # lui t1,0x0010
        0x352500c0,  # ori a1,t1,0xc0 = second path pointer
        0x3c0a8000,  # lui t2,0x8000
        0x354a0006,  # ori t2,t2,6 = LOADFILE RPC server constant
        0x3c080010,  # an unrelated pointer
        0x25040130,
    )
    for i, word in enumerate(instructions):
        struct.pack_into("<I", raw, 0x100 + i * 4, word)
    raw[0x180:0x180 + len(b"cdrom0:\\ARIKA.IRX;1\0")] = (
        b"cdrom0:\\ARIKA.IRX;1\0")
    raw[0x1c0:0x1c0 + len(b"rom0:LIBSD.irx\0")] = b"rom0:LIBSD.irx\0"
    raw[0x200:0x200 + len(b"not-a-module.dll\0")] = b"not-a-module.dll\0"

    report = analyzer.analyze_elf(bytes(raw))
    paths = report["module_paths"]
    assert len(paths) == 2, paths
    assert paths[0]["path"] == "cdrom0:\\ARIKA.IRX;1", paths
    assert paths[0]["guest_address"] == 0x00100080, paths
    assert paths[0]["file_offset"] == 0x180, paths
    assert [(p["lui_pc"], p["use_pc"], p["construction"])
            for p in paths[0]["references"]] == [
                (0x00100000, 0x00100004, "lui+addiu")
            ], paths
    assert paths[1]["path"] == "rom0:LIBSD.irx", paths
    assert paths[1]["references"][0]["computed_address"] == 0x001000c0
    assert len(report["loadfile_rpc_server_references"]) == 1, report
    assert report["loadfile_rpc_server_references"][0][
        "computed_address"] == 0x80000006

    with tempfile.TemporaryDirectory(prefix="sdbz-iop-path-test-") as tmp:
        root = Path(tmp)
        elf = root / "SLUS_214.42"
        destination = root / "report.json"
        elf.write_bytes(raw)
        result = subprocess.run(
            [sys.executable, str(TOOL), str(elf),
             "--json", str(destination)],
            capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
        parsed = json.loads(destination.read_text(encoding="utf-8"))
        assert parsed["module_paths"] == paths, parsed
        assert "NOT a confirmed load order" in result.stdout, result.stdout


def test_negative_low_and_branch_barrier():
    raw = make_elf(base=0x00108000)
    struct.pack_into("<I", raw, 0x100, 0x3c080011)
    struct.pack_into("<I", raw, 0x104, 0x25048020)
    raw[0x120:0x120 + len(b"host0:EXAMPLE.IRX\0")] = b"host0:EXAMPLE.IRX\0"
    report = analyzer.analyze_elf(bytes(raw))
    assert report["module_paths"][0]["guest_address"] == 0x00108020
    assert report["module_paths"][0]["references"][0][
        "computed_address"] == 0x00108020

    # A branch separates the LUI and ADDIU, so cross-block pairing is
    # not reliable. Do not invent a static xref from these instructions.
    struct.pack_into("<I", raw, 0x104, 0x10000001)
    struct.pack_into("<I", raw, 0x108, 0x25048020)
    report = analyzer.analyze_elf(bytes(raw))
    assert report["module_paths"][0]["references"] == [], report


def test_irx_callsite_recovery():
    # Direct LUI+ADDIU->JAL, then a second static call with a branch
    # between the high and low pieces (association is not a CFG proof).
    raw = make_elf(size=0x200)
    direct_target = 0x00123450
    indirect_target = 0x0017dea0
    instructions = (
        0x3c040010,  # lui a0,0x10
        0x24840080,  # addiu a0,a0,0x80
        (3 << 26) | (direct_target >> 2),
        0,
        0x3c040010,  # lui a0,0x10
        0x10000001,  # beq zero,zero,+1
        0x248400c0,  # addiu a0,a0,0xc0 (delay slot)
        (3 << 26) | (indirect_target >> 2),
        0,
        (3 << 26) | (0x00123470 >> 2),  # unrelated subsequent JAL
        0,
    )
    for i, word in enumerate(instructions):
        struct.pack_into("<I", raw, 0x100 + i * 4, word)
    raw[0x180:0x180 + 14] = b"rom0:TEST.IRX\0"
    raw[0x1c0:0x1c0 + 16] = b"host0:TEST2.IRX\0"
    report = analyzer.analyze_elf(bytes(raw))
    assert len(report["module_paths"]) == 2, report
    calls0 = report["module_paths"][0]["call_candidates"]
    calls1 = report["module_paths"][1]["call_candidates"]
    assert len(calls0) == 1 and len(calls1) == 1, report
    assert calls0[0]["call_pc"] == 0x00100008
    assert calls0[0]["call_target"] == direct_target
    assert calls0[0]["control_flow_ambiguous"] is False
    assert calls1[0]["call_pc"] == 0x0010001c
    assert calls1[0]["call_target"] == indirect_target
    assert calls1[0]["control_flow_ambiguous"] is True

    # Reuse the same high half through unrelated control flow, as the
    # US ELF does for the MCSERV and LIBSD path constants.
    raw = make_elf(base=0x004dce00, size=0x200)
    instructions = (
        0x3c04004e,  # lui a0,0x4e
        0x10000001,  # branch; static pairing is ambiguous
        0,
        0x2484ce50,  # addiu a0,a0,-0x31b0
        (3 << 26) | (indirect_target >> 2),
        0,
    )
    for i, word in enumerate(instructions):
        struct.pack_into("<I", raw, 0x100 + i * 4, word)
    raw[0x150:0x150 + 21] = b"cdrom0:\\MCSERV.IRX;1\0"
    report = analyzer.analyze_elf(bytes(raw))
    call = report["module_paths"][0]["call_candidates"][0]
    assert call["computed_address"] == 0x004dce50, call
    assert call["call_pc"] == 0x004dce10, call
    assert call["control_flow_ambiguous"] is True, call


def test_bad_headers_and_absent_strings():
    raw = make_elf()
    assert analyzer.analyze_elf(bytes(raw))["module_paths"] == []
    malformed = bytearray(raw)
    struct.pack_into("<H", malformed, 18, 3)
    try:
        analyzer.parse_elf(malformed)
    except ValueError as exc:
        assert "MIPS" in str(exc)
    else:
        raise AssertionError("non-MIPS ELF was accepted")
    struct.pack_into("<H", malformed, 18, 8)
    struct.pack_into("<I", malformed, 52 + 16, 0xffffffff)
    try:
        analyzer.parse_elf(malformed)
    except ValueError as exc:
        assert "PT_LOAD" in str(exc)
    else:
        raise AssertionError("out-of-bounds segment was accepted")


if __name__ == "__main__":
    test_discovery()
    test_negative_low_and_branch_barrier()
    test_irx_callsite_recovery()
    test_bad_headers_and_absent_strings()
    print("IOP module ELF path discovery: PASS")
