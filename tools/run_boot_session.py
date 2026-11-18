#!/usr/bin/env python3
"""Run bounded auto-expansion sessions, then capture one diagnostic boot trace.

Keep this wrapper separate from run_game.py: changing the translation driver
invalidates its ELF/object provenance cache, while changing orchestration
must not throw away previously compiled guest functions.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from iop_disc_catalog import build_catalog


LINKED_COUNT_PATTERN = re.compile(
    r"link_native_objects: PASS \((\d+) functions?,"
)


def read_function_count(path):
    """Read how many guest functions were actually committed."""
    report = read_report(path)
    count = report.get("function_count")
    return count if type(count) is int and count >= 0 else None


REPLAY_PATTERN = re.compile(r"== Executing translated EE code \(replay (\d+)\) ==")


def read_report(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return {}


def run_session(command, log_path, append=False):
    started = time.monotonic()
    result = subprocess.run(
        [str(item) for item in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    output = result.stdout or ""
    with log_path.open("a" if append else "w", encoding="utf-8") as log:
        log.write("+ " + subprocess.list2cmdline([str(item) for item in command]) + "\n")
        log.write(output)
    return result.returncode, output, time.monotonic() - started


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--expansion-passes", type=int, default=2)
    parser.add_argument("--expansion-per-pass", type=int, default=128)
    parser.add_argument("--no-recompile", action="store_true")
    parser.add_argument(
        "--disc-root", type=Path,
        help="Extracted CD-ROM root (default: directory containing the ELF)",
    )
    parser.add_argument(
        "--no-iop-catalog", action="store_true",
        help="Disable the non-executing diagnostic IOP disc catalog",
    )
    parser.add_argument(
        "--iop-auto-irx", action="append", default=[],
        metavar="GUEST_PATH_OR_NAME",
        help="Resolve a discovered IRX name from the game disc and append "
             "it to the explicit IOP bootstrap load list (repeat in "
             "provider-first order). Never infers execution order.",
    )
    parser.add_argument(
        "--driver", type=Path,
        default=Path(__file__).with_name("run_game.py"),
        help=argparse.SUPPRESS,
    )
    args, forwarded = parser.parse_known_args(argv)
    if not 1 <= args.expansion_passes <= 8:
        parser.error("--expansion-passes must be between 1 and 8")
    if not 1 <= args.expansion_per_pass <= 128:
        parser.error("--expansion-per-pass must be between 1 and 128")
    if any(item == "--trace" or item == "--link-only" or
           item == "--auto-expand" or item.startswith("--auto-expand=") or
           item == "--no-recompile" for item in forwarded):
        parser.error("trace, auto-expand, and no-recompile are managed by this wrapper")
    if not args.driver.is_file():
        parser.error(f"driver does not exist: {args.driver}")

    work = args.work_dir.resolve()
    report = args.report.resolve()
    work.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    catalog_path = work / "iop-disc-catalog.json"
    if args.no_iop_catalog and args.iop_auto_irx:
        parser.error("--iop-auto-irx requires disc catalog discovery")
    if args.iop_auto_irx and any(
            arg.startswith("--iop-irx=") or
            arg == "--iop-irx-list" or
            arg.startswith("--iop-irx-list=")
            for arg in forwarded):
        parser.error("--iop-auto-irx cannot mix with manual IOP IRX lists")
    catalog = None
    if not args.no_iop_catalog:
        try:
            catalog = build_catalog(args.elf, args.disc_root)
            catalog_path.write_text(
                json.dumps(catalog, indent=2) + "\n", encoding="utf-8"
            )
            print(
                f"IOP disc discovery: {catalog['resolved_count']}/"
                f"{len(catalog['candidates'])} candidate files resolved "
                f"({catalog_path}); no IOP modules started",
                flush=True,
            )
        except (OSError, ValueError) as exc:
            catalog_path.unlink(missing_ok=True)
            # Diagnostic discovery must not make EE boot or --no-recompile
            # fail when a disc is incomplete or a synthetic test ELF is
            # supplied. Never turn an invalid catalog into a load list.
            print(f"IOP disc discovery unavailable: {exc}", flush=True)

    for request in args.iop_auto_irx:
        if catalog is None:
            parser.error("IOP disc catalog could not be built")
        name = request.casefold()
        matches = [
            item for item in catalog["candidates"]
            if item["guest_path"].casefold() == name or
            item["guest_path"].replace("\\", "/").split("/")[-1]
                .split(";")[0].casefold() == name
        ]
        # A static ELF string is not a runtime load request or a
        # dependency graph. The caller chooses every bootstrap module.
        if len(matches) != 1 or matches[0]["status"] != "resolved":
            parser.error(
                f"IOP bootstrap IRX {request!r} is missing or ambiguous"
            )
        forwarded.append(f"--iop-irx={matches[0]['host_path']}")
        print(
            f"IOP explicit bootstrap selection: "
            f"{matches[0]['guest_path']} -> {matches[0]['host_path']}",
            flush=True,
        )

    expansion_log = work / "expansion.log"
    boot_log = work / "boot.log"
    metadata_path = work / "ir" / "ee_functions.json"
    expansion_log.write_text("", encoding="utf-8")
    base = [
        sys.executable, str(args.driver.resolve()), str(args.elf.resolve()),
        f"--work-dir={work}", f"--report={report}", *forwarded,
    ]

    reuse = args.no_recompile
    for index in range(args.expansion_passes):
        command = [*base, f"--auto-expand={args.expansion_per_pass}"]
        if reuse:
            command.append("--no-recompile")
        print(
            f"=== Expansion pass {index + 1}/{args.expansion_passes} "
            f"(up to {args.expansion_per_pass} new functions) ===",
            flush=True,
        )
        # Cache mode starts from a persisted count. Fresh translation
        # creates its initial linked baseline before replay zero.
        # A jump-table relink can compile many functions in one replay.
        prior_count = read_function_count(metadata_path) if reuse else None
        code, output, elapsed = run_session(command, expansion_log, append=True)
        state = read_report(report)
        replays = REPLAY_PATTERN.findall(output)
        last_replay = int(replays[-1]) if replays else -1
        final_pc = state.get("final_pc")
        pc_text = f"0x{final_pc:08x}" if isinstance(final_pc, int) else "unknown"
        if not reuse:
            initial_links = LINKED_COUNT_PATTERN.findall(
                output.split("== Executing translated EE code (replay ", 1)[0]
            )
            prior_count = int(initial_links[-1]) if initial_links else None
        current_count = read_function_count(metadata_path)
        added = (
            current_count - prior_count
            if prior_count is not None and current_count is not None and
               current_count >= prior_count
            else None
        )
        added_text = str(added) if added is not None else "unknown"
        print(
            f"pass {index + 1}: replay={last_replay} "
            f"new_functions={added_text} "
            f"dispatches={state.get('dispatches', '?')} "
            f"stop={pc_text} exit={code} elapsed={elapsed:.2f}s",
            flush=True,
        )

        if code not in (0, 3, 4):
            shutil.copyfile(expansion_log, boot_log)
            print(
                f"Expansion failed; diagnostics are in {boot_log}",
                file=sys.stderr, flush=True,
            )
            return code

        # The expansion cap counts newly emitted FUNCTIONS, not replays.
        # Prefer the metadata delta; use the legacy replay sentinel only
        # when the driver supplies no usable function-count metadata.
        # Unknown guest stops must not trigger an unbounded retry.
        hit_cap = (
            added >= args.expansion_per_pass
            if added is not None else last_replay == args.expansion_per_pass
        )
        exhausted = (
            code == 3 and hit_cap and
            state.get("exit_reason") == 3 and
            state.get("dispatch_status") == 0 and
            isinstance(final_pc, int) and final_pc != 0
        )
        if exhausted and index + 1 < args.expansion_passes:
            print(
                "Expansion bound reached; resuming from the validated "
                "translation cache.", flush=True,
            )
            reuse = True
            continue
        if exhausted:
            print(
                "Session expansion budget reached; compiled functions "
                "are preserved for the next local run.", flush=True,
            )
        break

    # Emit the *full* guest trace once. Intermediate replays don't need
    # repetitive dispatch logs, but the final snapshot must stay useful
    # for diagnosing the next kernel service or translation failure.
    print(f"=== Final traced replay: {boot_log} ===", flush=True)
    code, _, elapsed = run_session(
        [*base, "--no-recompile", "--auto-expand=0", "--trace"],
        boot_log,
    )
    state = read_report(report)
    final_pc = state.get("final_pc")
    pc_text = f"0x{final_pc:08x}" if isinstance(final_pc, int) else "unknown"
    print(
        f"Final trace: dispatches={state.get('dispatches', '?')} "
        f"stop={pc_text} exit={code} elapsed={elapsed:.2f}s. "
        f"Logs: {boot_log}, {expansion_log}",
        flush=True,
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
