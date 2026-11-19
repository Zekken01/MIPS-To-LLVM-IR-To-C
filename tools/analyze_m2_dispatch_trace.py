#!/usr/bin/env python3
"""M2.1 offline, read-only native-dispatch trace attribution.

Never treats native function-dispatch boundaries as R5900 instructions or
cycle timestamps. This is diagnostic evidence, not a guest execution engine.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

_DISPATCH = re.compile(
    r"^dispatch=(\d+) pc=0x([0-9a-fA-F]+) -> 0x([0-9a-fA-F]+) "
    r"reason=([A-Z_]+)\((\d+)\) status=(\d+)"
)
_EVENT = {
    "intc_irq": "HLE INTC IRQ:",
    "dmac_irq": "HLE DMAC IRQ:",
    "committed_sif_rpc": "IOP HLE RPC committed:",
}
_FIELD = re.compile(r"(?:^|\s)([a-z_]+)=([^\s]+)")


def _top(counter, limit, *, edge=False):
    result = []
    for key, value in sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))[:limit]:
        result.append(
            {"from_pc": f"0x{key[0]:08x}", "to_pc": f"0x{key[1]:08x}",
             "samples": value} if edge
            else {"pc": f"0x{key:08x}", "samples": value}
        )
    return result


def _digest(path):
    sha256 = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            sha256.update(chunk)
    return sha256.hexdigest()


def analyze(log_path, report, top=12, max_records=200000):
    """Summarize the existing --trace log and matching final runner report.

    Duplicate dispatch indices (yield / exception handoff) are preserved
    as a diagnostic count; the final matching record represents that index
    in the host-boundary histogram. No missing event is synthesized.
    """
    observations = {}
    record_limit_exceeded = False
    native_runner_command = None
    raw_dispatch_records = 0
    repeated_indices = 0
    events = {key: {"count": 0, "first": None, "last": None}
              for key in _EVENT}
    preceding_dispatch = None

    with Path(log_path).open("r", encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, 1):
            # Capture the command that actually invoked the *final* runner,
            # not an assumed launcher option or a stale GPU snapshot file.
            if line.lstrip().startswith("+ ") and "ee_game_runner" in line:
                native_runner_command = line.strip()[2:]
            match = _DISPATCH.match(line)
            if match:
                index = int(match.group(1))
                if index <= 0 or index > int(report["dispatches"]):
                    continue
                raw_dispatch_records += 1
                if index in observations:
                    repeated_indices += 1
                if index not in observations and len(observations) >= max_records:
                    record_limit_exceeded = True
                else:
                    observations[index] = (
                        int(match.group(2), 16),
                        int(match.group(3), 16),
                        match.group(4),
                    )
                preceding_dispatch = index

            for key, needle in _EVENT.items():
                if needle not in line:
                    continue
                # This is the nearest preceding log dispatch, not an
                # authoritative event-cycle or even a guest instruction PC.
                fields = dict(_FIELD.findall(line))
                observation = {
                    "line": line_number,
                    "preceding_trace_dispatch": preceding_dispatch,
                    "fields": {name: fields[name] for name in (
                        "cause", "channel", "seq", "server", "method",
                        "path", "load_status", "started", "epc"
                    ) if name in fields},
                }
                events[key]["count"] += 1
                if events[key]["first"] is None:
                    events[key]["first"] = observation
                events[key]["last"] = observation

    pc_counts = Counter()
    edge_counts = Counter()
    reason_counts = Counter()
    phases = []
    limit_dispatch = int(report["dispatches"])
    boundaries = [0, min(limit_dispatch, 6000),
                  min(limit_dispatch, 12500), limit_dispatch]
    for start, end in zip(boundaries, boundaries[1:]):
        if end <= start:
            continue
        phase_pcs = Counter()
        phase_edges = Counter()
        count = 0
        for index, (from_pc, to_pc, _) in observations.items():
            if start < index <= end:
                count += 1
                phase_pcs[from_pc] += 1
                phase_edges[(from_pc, to_pc)] += 1
        phases.append({
            "from_dispatch_exclusive": start, "to_dispatch_inclusive": end,
            "observed_dispatch_indices": count,
            "hot_pcs": _top(phase_pcs, top),
            "hot_transitions": _top(phase_edges, top, edge=True),
        })

    for from_pc, to_pc, reason in observations.values():
        pc_counts[from_pc] += 1
        edge_counts[(from_pc, to_pc)] += 1
        reason_counts[reason] += 1

    return {
        "schema": "ee-m2-dispatch-log-analysis-v1",
        "source": "existing_guest_trace_and_final_report",
        "observation_unit": "last_native_dispatch_record_per_index",
        "dispatches_reported": limit_dispatch,
        "trace_records": raw_dispatch_records,
        "distinct_dispatch_indices": len(observations),
        "repeated_dispatch_indices": repeated_indices,
        "indices_not_observed": max(0, limit_dispatch - len(observations)),
        "record_limit": max_records,
        "record_limit_exceeded": record_limit_exceeded,
        "final_runner_command": native_runner_command,
        "final_runner_flags": {
            "webgpu_snapshot_requested": (
                "--gs-webgpu-snapshot" in native_runner_command
                if native_runner_command is not None else None),
            "cpu_display_snapshot_requested": (
                "--gs-snapshot" in native_runner_command
                if native_runner_command is not None else None),
            "cpu_upload_snapshot_requested": (
                "--gs-upload-snapshot" in native_runner_command
                if native_runner_command is not None else None),
            "reference_compat_clock_requested": (
                "--ee-clock=reference-compat" in native_runner_command
                if native_runner_command is not None else None),
        },
        "event_counts": events,
        "guest_clock": {
            "source": report.get("ee_cycle_source"),
            "gs_fields_started": report.get("gs_fields_started"),
            "scheduler_yields": report.get("scheduler_yields"),
            "guest_instruction_attempts": report.get("guest_instruction_attempts"),
        },
        "hot_pcs": _top(pc_counts, top),
        "hot_transitions": _top(edge_counts, top, edge=True),
        "exit_reasons": dict(sorted(reason_counts.items())),
        "phases": phases,
        "limitations": [
            "trace_has_host_dispatch_boundaries_not_every_guest_instruction",
            "preceding_trace_dispatch_for_events_is_not_a_cycle_timestamp",
            "late_events_must_be_checked_against_guest_owned_state",
            "absence_from_log_is_not_proof_that_a_hardware_event_could_not_occur",
            "pc_to_wait_predicate_requires_guest_elf_disassembly",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--max-records", type=int, default=200000)
    parser.add_argument("--catalog", type=Path,
                        help="explicit matching iop-disc-catalog JSON; not auto-inferred")
    parser.add_argument("--revision",
                        help="explicit verified Git revision for this historical batch")
    arguments = parser.parse_args()
    if not 1 <= arguments.top <= 64 or arguments.max_records < 1:
        parser.error("require 1 <= --top <= 64 and --max-records >= 1")
    with arguments.report.open(encoding="utf-8") as source:
        report = json.load(source)
    if report.get("schema") != "ee-game-run-report-v1" or "dispatches" not in report:
        parser.error("expected an ee-game-run-report-v1 final runner JSON")
    result = analyze(arguments.boot_log, report, arguments.top, arguments.max_records)
    result["inputs"] = {
        "boot_log": arguments.boot_log.name,
        "boot_log_sha256": _digest(arguments.boot_log),
        "report": arguments.report.name,
        "report_sha256": _digest(arguments.report),
    }
    result["revision"] = arguments.revision
    if arguments.catalog is not None:
        with arguments.catalog.open(encoding="utf-8") as source:
            catalog = json.load(source)
        if catalog.get("schema") != "iop-disc-catalog-v1":
            parser.error("expected iop-disc-catalog-v1, not arbitrary JSON")
        result["inputs"]["catalog"] = arguments.catalog.name
        result["inputs"]["catalog_sha256"] = _digest(arguments.catalog)
        result["catalog_evidence"] = {
            "elf_sha256": catalog.get("elf_sha256"),
            "elf_sha256_provenance": "catalog_field_not_rehashed_elf",
            "rpc_commits_reported": catalog.get("runtime_rpc_commit_count"),
            "rpc_commits_logged": result["event_counts"]["committed_sif_rpc"]["count"],
            "rpc_count_agrees": (
                catalog.get("runtime_rpc_commit_count")
                == result["event_counts"]["committed_sif_rpc"]["count"]),
        }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"M2 trace: {result['distinct_dispatch_indices']}/{result['dispatches_reported']} "
          f"dispatch indices, {result['event_counts']['intc_irq']['count']} INTC, "
          f"{result['event_counts']['committed_sif_rpc']['count']} RPCs; "
          f"output={arguments.output}")


if __name__ == "__main__":
    main()
