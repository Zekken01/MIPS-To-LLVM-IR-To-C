#!/usr/bin/env python3
"""Compare per-pair VU0 architectural traces from this project and PCSX2.

Each input is JSON Lines. The first record is a header:
  {"schema":"mips-to-llvm-vu0-trace-v1","kind":"header",
   "program_sha256":"...", "initial_state":{...}, "engine":"..."}
Subsequent records are:
  {"kind":"step","step":0,"pc":0,"lower":0,"upper":0,
   "state":{"vi":[32 integers], "cycle":1, "micro_pc":8, "tpc":1,
            "branch":0, "branch_pc":0, "delay_branch_pc":0,
            "take_delay_branch":false, "vi_backup_cycles":0,
            "vi_backup_register":0, "vi_backup_old_value":0,
            "e_bit":0, "m_bit":0, "vpu_status":1, "running":true}}

The adapter producing a trace is responsible for sampling after the same
architectural pair boundary on both engines. No floating-point tolerance is
needed for the currently supported integer/branch subset.
"""
import argparse
import json
import sys
from pathlib import Path

SCHEMA = "mips-to-llvm-vu0-trace-v1"
STATE_FIELDS = (
    "vi", "cycle", "micro_pc", "tpc", "branch", "branch_pc",
    "delay_branch_pc", "take_delay_branch", "vi_backup_cycles",
    "vi_backup_register", "vi_backup_old_value", "e_bit", "m_bit",
    "vpu_status", "running",
)


class TraceError(Exception):
    pass


def read_trace(path):
    records = []
    try:
        with Path(path).open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise TraceError(
                        f"{path}:{line_number}: invalid JSON: {exc.msg}"
                    ) from exc
                if not isinstance(record, dict):
                    raise TraceError(
                        f"{path}:{line_number}: record must be an object"
                    )
                records.append(record)
    except OSError as exc:
        raise TraceError(f"cannot read {path}: {exc}") from exc

    if not records or records[0].get("kind") != "header":
        raise TraceError(f"{path}: missing header record")
    header = records[0]
    if header.get("schema") != SCHEMA:
        raise TraceError(
            f"{path}: unsupported schema {header.get('schema')!r}"
        )
    if not isinstance(header.get("program_sha256"), str):
        raise TraceError(f"{path}: header has no program_sha256")
    if not isinstance(header.get("initial_state"), dict):
        raise TraceError(f"{path}: header has no initial_state object")

    steps = []
    for index, record in enumerate(records[1:]):
        if record.get("kind") != "step":
            raise TraceError(f"{path}: record {index + 2} is not a step")
        for field in ("pc", "lower", "upper"):
            value = record.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise TraceError(
                    f"{path}: record {index + 2} has invalid {field}: {value!r}"
                )
        step = record.get("step")
        if not isinstance(step, int) or isinstance(step, bool) or step != index:
            raise TraceError(
                f"{path}: expected step {index}, got {step!r}"
            )
        state = record.get("state")
        if not isinstance(state, dict):
            raise TraceError(f"{path}: step {step} has no state object")
        missing = [field for field in STATE_FIELDS if field not in state]
        if missing:
            raise TraceError(
                f"{path}: step {step} missing state fields: {', '.join(missing)}"
            )
        if not isinstance(state["vi"], list) or len(state["vi"]) != 32:
            raise TraceError(f"{path}: step {step} vi must contain 32 entries")
        steps.append(record)
    return header, steps


def compare_traces(left_path, right_path):
    left_header, left_steps = read_trace(left_path)
    right_header, right_steps = read_trace(right_path)

    for field in ("program_sha256", "initial_state"):
        if left_header[field] != right_header[field]:
            raise TraceError(f"header mismatch: {field}")

    if len(left_steps) != len(right_steps):
        return [
            f"step count differs: {left_path}={len(left_steps)}, "
            f"{right_path}={len(right_steps)}"
        ]

    mismatches = []
    for index, (left, right) in enumerate(zip(left_steps, right_steps)):
        for field in ("pc", "lower", "upper"):
            if left.get(field) != right.get(field):
                mismatches.append(
                    f"step {index} instruction {field}: "
                    f"{left.get(field)!r} != {right.get(field)!r}"
                )
        for field in STATE_FIELDS:
            a = left["state"][field]
            b = right["state"][field]
            if a == b:
                continue
            if field == "vi":
                for register, (va, vb) in enumerate(zip(a, b)):
                    if va != vb:
                        mismatches.append(
                            f"step {index} VI{register}: 0x{va:08x} != 0x{vb:08x}"
                        )
            else:
                mismatches.append(
                    f"step {index} {field}: {a!r} != {b!r}"
                )
    return mismatches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", help="PCSX2 interpreter JSONL trace")
    parser.add_argument("candidate", help="recompiler JSONL trace")
    args = parser.parse_args()
    try:
        mismatches = compare_traces(args.reference, args.candidate)
    except TraceError as exc:
        print(f"trace error: {exc}", file=sys.stderr)
        return 2

    if mismatches:
        print(f"VU0 trace mismatch: {len(mismatches)} difference(s)")
        for mismatch in mismatches[:100]:
            print(f"  {mismatch}")
        if len(mismatches) > 100:
            print(f"  ... {len(mismatches) - 100} additional difference(s)")
        return 1

    print("VU0 traces match exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
