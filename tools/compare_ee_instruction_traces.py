#!/usr/bin/env python3
"""Validate a native EE instruction-PC trace and optionally align a reference.

Native:    attempt,pc,opcode_word  (bounded guest instruction attempts, NOT cycles)
Reference: cycle,pc,opcode_word    (independently recorded EE issue timestamps)

The reference must already have the same PC filter and start alignment. This
utility never manufactures a cycle count or authorizes GS scanout scheduling.
"""

import argparse
import csv
import json
import re
import sys
from pathlib import Path


MARKER = re.compile(
    r"# truncated=(true|false) range=(0x[0-9a-fA-F]{8}):(0x[0-9a-fA-F]{8})"
)


def parse_uint(value, bits, label):
    try:
        number = value if isinstance(value, int) else int(value, 0)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{label}: invalid integer {value!r}") from error
    if not 0 <= number <= (1 << bits) - 1:
        raise ValueError(f"{label}: outside unsigned {bits}-bit range: {value!r}")
    return number


def read_table(path, headers, ordinal_column):
    """Read a strict stream with monotonic ordinals, allowing filtered gaps."""
    rows = []
    comments = []
    with path.open("r", encoding="utf-8-sig", newline="") as source:
        header = source.readline().strip()
        if header != ",".join(headers):
            raise ValueError(f"{path}: expected CSV header {','.join(headers)!r}")
        reached_footer = False
        for lineno, raw in enumerate(source, start=2):
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                comments.append(line)
                reached_footer = True
                continue
            if reached_footer:
                raise ValueError(f"{path}:{lineno}: record after footer")
            fields = next(csv.reader([line]))
            if len(fields) != len(headers):
                raise ValueError(f"{path}:{lineno}: expected {len(headers)} fields")
            values = []
            for key, text in zip(headers, fields):
                number = parse_uint(text, 64 if key in ("attempt", "cycle") else 32,
                                    f"{path}:{lineno}:{key}")
                if key == "pc" and number % 4:
                    raise ValueError(f"{path}:{lineno}: unaligned guest PC")
                values.append(number)
            if rows and values[0] <= rows[-1][0]:
                order = "strictly increasing" if ordinal_column == "attempt" else "nondecreasing"
                if ordinal_column == "cycle" and values[0] == rows[-1][0]:
                    pass  # A trustworthy reference may record two issued instructions per cycle.
                else:
                    raise ValueError(f"{path}:{lineno}: {ordinal_column} must be {order}")
            rows.append(tuple(values))
    if not rows:
        raise ValueError(f"{path}: no records")
    return rows, comments


def read_native(path):
    rows, comments = read_table(path, ("attempt", "pc", "opcode_word"), "attempt")
    if len(comments) != 1:
        raise ValueError(f"{path}: expected one bounded-trace footer")
    match = MARKER.fullmatch(comments[0])
    if match is None:
        raise ValueError(f"{path}: malformed truncated/range footer")
    lower, upper = int(match[2], 16), int(match[3], 16)
    if lower > upper or (lower | upper) & 3:
        raise ValueError(f"{path}: invalid PC filter range")
    for attempt, pc, opcode in rows:
        if not lower <= pc <= upper:
            raise ValueError(f"{path}: guest PC 0x{pc:08x} outside filter")
    return rows, {"truncated": match[1] == "true", "range_start": f"0x{lower:08x}",
                  "range_end": f"0x{upper:08x}"}


def check_loop(rows, pattern):
    """Prove only the *captured* contiguous segment, not any truncated tail."""
    first = next((i for i, row in enumerate(rows) if row[1] == pattern[0]), None)
    if first is None:
        raise ValueError(f"loop entry 0x{pattern[0]:08x} absent from capture")
    segment = rows[first:]
    for i, row in enumerate(segment):
        expected = pattern[i % len(pattern)]
        if row[1] != expected:
            raise ValueError(
                f"loop sequence mismatch at captured record {first+i}: "
                f"guest PC 0x{row[1]:08x} != 0x{expected:08x}"
            )
        if i and row[0] != segment[i - 1][0] + 1:
            raise ValueError(f"non-contiguous attempt sequence in captured loop at {first+i}")
    if len(segment) % len(pattern):
        raise ValueError("capture ends mid-pattern; report partial coverage explicitly")
    return {"prefix_entries": first, "captured_loop_attempts": len(segment),
            "captured_complete_iterations": len(segment) // len(pattern),
            "loop_first_attempt": segment[0][0],
            "loop_last_captured_attempt": segment[-1][0]}


def read_reference(path):
    rows, comments = read_table(path, ("cycle", "pc", "opcode_word"), "cycle")
    if comments:
        raise ValueError(f"{path}: no footer is defined for externally measured cycles")
    return rows


def compare_reference(native, reference, truncated):
    if len(reference) < len(native):
        raise ValueError("reference trace shorter than native capture")
    if not truncated and len(reference) != len(native):
        raise ValueError("native trace complete, but reference has extra records")
    for index, (ours, external) in enumerate(zip(native, reference)):
        if ours[1:] != external[1:]:
            raise ValueError(
                f"reference mismatch at record {index}: "
                f"native PC/opcode 0x{ours[1]:08x}/0x{ours[2]:08x}, "
                f"reference 0x{external[1]:08x}/0x{external[2]:08x}"
            )
    # Cycle numbers are explicitly supplied by the external measurement.
    # Do not extrapolate beyond the matched record window.
    return {"records_compared": len(native),
            "reference_has_additional_records": len(reference) > len(native),
            "reference_first_cycle": reference[0][0],
            "reference_last_matched_cycle": reference[len(native)-1][0],
            "reference_matched_cycle_span": reference[len(native)-1][0] - reference[0][0]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path, help="native bounded attempt/PC/opcode CSV")
    parser.add_argument("--report", type=Path, help="corresponding terminal EE JSON report")
    parser.add_argument("--loop-pcs", help="exact comma-separated pattern to verify inside the captured stream")
    parser.add_argument("--reference", type=Path, help="independent cycle/PC/opcode CSV, already filtered and aligned")
    parser.add_argument("--reference-provenance", help="reference source, hardware/tool version, capture settings")
    parser.add_argument("--json", type=Path, help="write machine-readable validation summary")
    args = parser.parse_args(argv)
    if (args.reference is None) != (args.reference_provenance is None):
        parser.error("--reference requires --reference-provenance, and vice versa")
    try:
        native, footer = read_native(args.native)
        result = {"schema": "ee-instruction-order-validation-v1",
                  "attempts_are_cycles": False,
                  "native_entries": len(native),
                  "first_captured_attempt": native[0][0],
                  "last_captured_attempt": native[-1][0],
                  **footer}
        if args.report is not None:
            report = json.loads(args.report.read_text(encoding="utf-8"))
            total_attempts = parse_uint(report.get("guest_instruction_attempts"), 64,
                                        "report.guest_instruction_attempts")
            if total_attempts < native[-1][0]:
                raise ValueError("terminal report ended before last captured attempt")
            if report.get("snapshot_kind") != "stopped":
                raise ValueError("report is not a terminal EE stop")
            result["terminal"] = {
                "exit_name": report.get("exit_name"),
                "final_pc": f"0x{parse_uint(report.get('final_pc'),32,'final_pc'):08x}",
                "dispatches": report.get("dispatches"),
                "guest_instruction_attempts": total_attempts,
                "attempts_after_last_captured": total_attempts - native[-1][0],
            }
        if args.loop_pcs is not None:
            pattern = [parse_uint(s.strip(), 32, "loop guest PC")
                       for s in args.loop_pcs.split(",")]
            if not pattern or any(pc % 4 for pc in pattern):
                raise ValueError("loop PC pattern must contain aligned addresses")
            result["verified_captured_loop"] = check_loop(native, pattern)
        if args.reference is not None:
            result["reference"] = {
                "claimed_provenance": args.reference_provenance,
                "external_timing_independently_verified": False,
                **compare_reference(native, read_reference(args.reference), footer["truncated"]),
            }
        if args.json is not None:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"compare_ee_instruction_traces: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
