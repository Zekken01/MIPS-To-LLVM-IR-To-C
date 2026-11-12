# VU0 interpreter differential trace format

The supported VU0 lower integer/branch subset currently has PCSX2-derived
reference models, but those models are not an execution of PCSX2. This document
defines the interchange format for the next validation stage.

## Pinned reference

Use PCSX2 revision
`82cac38b2af19d593dedfd9ddaa63448e7ec493c` (the revision audited for the
lower-op/interlock/pipe/VI-backup ordering correction).

Relevant reference implementation:

- `pcsx2/VU0microInterp.cpp`: `_vu0Exec`, `vu0Exec`, and
  `InterpVU0::Execute`.
- `pcsx2/VUops.cpp`: lower-op metadata, `_vuTestLowerStalls`,
  `_vuTestPipes`, and VI backup accessors.

The adapter must use the interpreter, not microVU/JIT. It must disable speed
hacks that alter cycle accounting and execute one micro pair at a time. Capture
state after `vu0Exec` returns, when the interpreter's cycle has advanced and
branch/E retirement has completed. The micro-PC must be normalized to the
byte-address representation used by this project.

## JSONL contract

A trace is UTF-8 JSON Lines. The first line is a header:

```json
{
  "schema": "mips-to-llvm-vu0-trace-v1",
  "kind": "header",
  "engine": "pcsx2-interpreter",
  "revision": "82cac38b2af19d593dedfd9ddaa63448e7ec493c",
  "program_sha256": "sha256-of-the-4096-byte-micro-memory",
  "initial_state": {
    "vi": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "tpc": 0,
    "cycle": 0
  }
}
```

Each subsequent line records one completed pair. `lower` and `upper` are the
raw 32-bit instruction words. `state.vi` contains all 32 architectural VI
register values (including the special-register slots). The remaining fields
are the state needed to validate the currently implemented scalar/branch
subset:

```json
{
  "kind": "step",
  "step": 0,
  "pc": 0,
  "lower": 1006634032,
  "upper": 0,
  "state": {
    "vi": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "cycle": 1,
    "micro_pc": 8,
    "tpc": 1,
    "branch": 0,
    "branch_pc": 0,
    "delay_branch_pc": 0,
    "take_delay_branch": false,
    "vi_backup_cycles": 0,
    "vi_backup_register": 0,
    "vi_backup_old_value": 0,
    "e_bit": 0,
    "m_bit": false,
    "vpu_status": 1,
    "running": true
  }
}
```

The shared fixture now provides a deterministic 37-pair program. The
candidate producer executes one runtime pair per exact-cycle callback and
writes a trace. The pinned PCSX2 adapter executes one interpreter pair through
vu0Exec(&VU0) and writes the same format when VU0_TRACE_OUTPUT is set. The
trace records the executed pair PC as well as both instruction words.

The comparator checks program identity and initial state, then compares every
pair PC, instruction word and listed architectural field exactly after each pair.
Engine name and source revision are informational and intentionally need not
match. A mismatch reports the first differing VI register or state field.

## Comparator

After generating one trace from the pinned PCSX2 interpreter and one from this
project's VU0 runtime, run:

```sh
python3 tools/compare_vu0_traces.py pcsx2.jsonl candidate.jsonl
```

Exit codes: 0 means exact match, 1 means architectural mismatch, and 2 means
malformed/incompatible traces.

## Current implementation and limitation

The repository now contains all pieces needed to produce both sides of the
trace:

- A shared deterministic 37-pair microprogram fixture.
- The in-tree candidate trace executable, built as `ee_vu0_trace_candidate`.
- A pinned PCSX2 GoogleTest adapter that captures one pair after each
  `vu0Exec(&VU0)` call when `VU0_TRACE_OUTPUT` is set.
- The JSONL comparator and regression tests, including executed-PC comparison.

CTest validates the candidate trace path and its expected 30 executed pairs.
To run the real cross-engine comparison, build a PCSX2 checkout at the pinned
revision, install the adapter with
`tools/vu0_diff/integrate_pcsx2_test.py`, and follow the commands in
`tools/vu0_diff/README.md`.

The PCSX2 trace has not yet been produced and compared in this environment.
Do not claim live differential validation until that comparison succeeds.
