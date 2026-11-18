#!/usr/bin/env python3
"""Regression tests for PS2 disc IRX path resolution and startup safety."""

import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "iop_disc_catalog", TOOLS / "iop_disc_catalog.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_mips_elf(path):
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", header, 18, 8)
    path.write_bytes(header)


def make_test_elf(paths):
    image = bytearray(0x100 + 0x500)
    image[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", image, 16, 2, 8, 1)
    struct.pack_into("<I", image, 28, 52)
    struct.pack_into("<HH", image, 40, 52, 32)
    struct.pack_into("<H", image, 44, 1)
    struct.pack_into("<IIIIIIII", image, 52,
                     1, 0x100, 0x100000, 0x100000, 0x500, 0x500, 5, 4)
    at = 0x160
    for path in paths:
        encoded = path.encode("ascii") + b"\x00"
        image[at:at + len(encoded)] = encoded
        at += len(encoded) + 8
    return image


def rpc_commit(sequence, *, cid=0x8000000A, server=0x80000006,
               method=0, path="(none)", load=2, link=1,
               module=0, thread=0, started=0):
    return (
        f"IOP HLE RPC committed: seq={sequence} cid=0x{cid:08x} "
        f"server=0x{server:08x} method=0x{method:08x} "
        f"client=0x00564d40 send=512 recv=8 result=0x00000000 "
        f"path={path} load_status={load} link_status={link} "
        f"module={module} thread={thread} started={started} "
        f"irx_entry=00000634 irx_gp=00008fb0 irx_image=00000ff0 "
        f"unresolved=(none)#0"
    )


def sample_commits():
    return "\n".join([
        rpc_commit(1, cid=0x80000009, method=0),
        rpc_commit(2, method=0xff),
        rpc_commit(3, path=r"cdrom0:\PADMAN.IRX;1", load=0,
                   link=0, module=1, thread=1, started=1),
        rpc_commit(4, server=0x80000593, method=0x22),
        rpc_commit(5, path=r"cdrom0:\MISSING.IRX;1", load=2,
                   link=1, module=0, thread=0, started=0),
    ]) + "\n"


def expect_error(callback, fragment):
    try:
        callback()
    except ValueError as error:
        assert fragment in str(error), (fragment, error)
    else:
        raise AssertionError(f"expected failure containing {fragment!r}")


def main():
    assert MODULE.parse_cdrom_path(r"cdrom0:\PADMAN.IRX;1") == (
        "PADMAN.IRX;1",
    )
    assert MODULE.parse_cdrom_path("cdrom0:/DATA/ARIKA.IRX") == (
        "DATA", "ARIKA.IRX",
    )
    for guest in (
        "rom0:PADMAN.IRX",
        "cdrom0:/../secret.irx",
        "cdrom0:/DIR/../../secret.irx",
        "cdrom0:C:/system.irx",
        "cdrom0:/PADMAN.DAT",
        "cdrom0:/a//mod.irx",
        "cdrom0:/a/\nmod.irx",
    ):
        assert MODULE.parse_cdrom_path(guest) is None, guest

    with tempfile.TemporaryDirectory(prefix="iop-disc-catalog-") as temp:
        root = Path(temp)
        disc = root / "PS2_GAME"
        disc.mkdir()
        (disc / "DATA").mkdir()
        write_mips_elf(disc / "padman.irx")
        write_mips_elf(disc / "DATA" / "ArIkA.IrX;1")
        elf = disc / "SLUS_214.42"
        elf.write_bytes(make_test_elf([
            r"cdrom0:\PADMAN.IRX;1",
            "cdrom0:/DATA/ARIKA.IRX;1",
            r"cdrom0:\MISSING.IRX;1",
            "rom0:LOADCORE.IRX",
        ]))
        result = MODULE.build_catalog(elf)
        assert result["schema"] == "iop-disc-catalog-v1"
        assert [item["status"] for item in result["candidates"]] == [
            "resolved", "resolved", "missing", "unsupported-device",
        ], result
        assert result["resolved_count"] == 2
        assert result["runtime_load_count"] == 0
        assert result["runtime_evidence_source"] == "static-only"
        original_paths = [item["guest_path"] for item in result["candidates"]]
        updated = MODULE.reconcile_runtime_loadfile(result, sample_commits())
        assert updated["runtime_rpc_commit_count"] == 5
        assert updated["runtime_load_attempt_count"] == 2
        assert updated["runtime_load_count"] == 1
        assert updated["runtime_evidence_source"] == (
            "final-replay-committed-sifrpc"
        )
        assert updated["runtime_load_events"][0]["success"]
        assert updated["runtime_load_events"][0]["module_id"] == 1
        assert not updated["runtime_load_events"][1]["success"]
        assert [item["guest_path"] for item in updated["candidates"]] == (
            original_paths
        )
        before_rejection = json.dumps(updated, sort_keys=True)
        expect_error(
            lambda: MODULE.reconcile_runtime_loadfile(
                updated, sample_commits() + "IOP HLE RPC committed: bad\n"
            ), "unparseable"
        )
        assert json.dumps(updated, sort_keys=True) == before_rejection
        expect_error(
            lambda: MODULE.reconcile_runtime_loadfile(
                updated, rpc_commit(2) + "\n" + rpc_commit(1)
            ), "non-monotonic"
        )
        expect_error(
            lambda: MODULE.reconcile_runtime_loadfile(
                updated, rpc_commit(1) + "\n" + rpc_commit(3)
            ), "gapped"
        )
        assert json.dumps(updated, sort_keys=True) == before_rejection
        assert result["candidates"][0]["host_path"] == str(
            disc / "padman.irx"
        )
        assert result["candidates"][1]["host_path"] == str(
            disc / "DATA" / "ArIkA.IrX;1"
        )

        output = root / "catalog.json"
        proc = subprocess.run([
            sys.executable, str(TOOLS / "iop_disc_catalog.py"),
            str(elf), "--json", str(output)
        ], text=True, capture_output=True, check=False)
        assert proc.returncode == 0, proc.stderr
        assert "no modules loaded or executed" in proc.stdout
        assert json.loads(output.read_text())["resolved_count"] == 2

        # The boot wrapper passes explicitly selected *guest names* to
        # the existing IOP module loader as resolved host paths. It
        # does not start every static ELF candidate.
        session_spec = importlib.util.spec_from_file_location(
            "run_boot_session", TOOLS / "run_boot_session.py"
        )
        session = importlib.util.module_from_spec(session_spec)
        session_spec.loader.exec_module(session)
        work = root / "work"
        report = work / "first-boot.json"
        calls = []

        def fake_run(command, log_path, append=False):
            calls.append(list(map(str, command)))
            report.write_text(
                json.dumps({
                    "dispatches": 5,
                    "final_pc": 0x1750b4,
                    "exit_reason": 4,
                }), encoding="utf-8"
            )
            output = "EE stopped\n"
            if "--trace" in command:
                output += sample_commits()
            with log_path.open("a" if append else "w") as log:
                log.write(output)
            return 0, output, 0.001

        session.run_session = fake_run
        result_code = session.main([
            str(elf), "--work-dir", str(work), "--report", str(report),
            "--expansion-passes=1", "--expansion-per-pass=1",
            "--no-recompile", "--iop-auto-irx=PADMAN.IRX",
            "--driver", str(TOOLS / "run_game.py"),
        ])
        assert result_code == 0 and len(calls) == 2, calls
        expected = f"--iop-irx={disc / 'padman.irx'}"
        assert all(expected in call for call in calls), calls
        assert "--no-recompile" in calls[0]
        saved = json.loads(
            (work / "iop-disc-catalog.json").read_text(encoding="utf-8")
        )
        assert saved["runtime_load_count"] == 1, saved
        assert saved["runtime_load_attempt_count"] == 2, saved
        assert saved["runtime_rpc_commit_count"] == 5, saved
        assert saved["runtime_evidence_source"] == (
            "final-replay-committed-sifrpc"
        )

        # Case-insensitive ambiguity is an error even on Linux.
        write_mips_elf(disc / "PADMAN.IRX;1")
        expect_error(
            lambda: MODULE.build_catalog(elf), "ambiguous"
        )
        (disc / "PADMAN.IRX;1").unlink()

        # A symlink inside the disc cannot be used to access host files.
        (disc / "padman.irx").unlink()
        try:
            (disc / "padman.irx").symlink_to(
                root / "secret.irx"
            )
        except (OSError, NotImplementedError):
            pass
        else:
            expect_error(
                lambda: MODULE.build_catalog(elf), "symlink"
            )
            (disc / "padman.irx").unlink()

        # A file with an .IRX name is not necessarily an IRX.
        (disc / "padman.irx").write_bytes(b"not an IRX" * 8)
        expect_error(
            lambda: MODULE.build_catalog(elf), "MIPS IRX"
        )

    print("IOP disc candidate resolution: PASS")


if __name__ == "__main__":
    main()
