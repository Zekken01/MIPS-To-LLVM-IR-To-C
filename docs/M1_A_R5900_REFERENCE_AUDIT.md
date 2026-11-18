# M1-A R5900 timing reference audit (implementation, NOT clock validation)

**Branch:** `feature/sdbz-hle-first-bootstrap` · **PR:** #142 (draft)
**Scope:** off-line instruction issue replay only. The native runner's EE clock
source remains unavailable; this document is **not** M1-B game-boot evidence.

## Primary architectural reference

- Sony Computer Entertainment, *EE Core User's Manual*, version 6.0:
  https://docs.alexrp.com/mips/ee.pdf
- Table 1-2 (printed p.25): logical Pipe 0/1 instruction categories.
- Table 1-3 (printed p.26): concurrent issue combinations, including X
  (forbidden) and Y (issue together but one-cycle A-stage resource stall).
- Table 2-12 (printed p.58): instruction execution latency and throughput.
- Section 8.3.2 (printed pp.158–159): FCR31 condition bit and arithmetic
  cause/sticky status flags.
- PCSX2 opcode decoder/implementation is used only as a cross-check for
  *architectural instruction behavior*, **not** as a hardware cycle oracle:
  https://github.com/PCSX2/pcsx2

The EE manual explicitly notes that a complete program's cycle count also
depends on dynamic pairing and cache misses. A table of *instruction*
latencies is not a validated whole-program cycle producer.

## Manual-derived, hardware-reference constants (not installed)

| Instruction category | Latency (cycles) | Throughput (cycles) |
|---|---:|---:|
| Integer add/sub/logic, shift, branch | 1 | 1 |
| MULT/MULTU, MULT1/MULTU1 | 4 | 2 |
| DIV/DIVU, DIV1/DIVU1 | 37 | 37 |
| MADD/MADDU, MADD1/MADDU1 | 4 | 2 |
| Load with no cache miss | 1 | 1 |
| MTC1, MFC1 | 2 | 1 |
| COP1 scalar add/sub/compare, multiply, conversion | 4 | 1 |
| DIV.S | 8 | 7 |
| SQRT.S | 8 | 7 |
| RSQRT.S | 14 | 13 |

These are a reference to audit candidate latencies, **not** auto-loaded
values. The existing explicit profile remains opt-in and synthetic. In
particular, the manual's throughput column is **not yet represented** by
the issue probe's functional-unit occupancy scoreboard. Therefore repeated
MULT/DIV/COP1 division timing may still be wrong even if every result
latency is correct.

## Implemented in this branch

1. Separate logical issue classes for ALU/branch (either pipe),
   load/store and COP1 move (Pipe 1), scalar COP1 operate and MAC0
   (Pipe 0), and MAC1 (Pipe 1). Pair only if two logical pipes can accept
   the instruction categories, there are no tracked dependencies, and
   Table 1-3 does not mark the pair as X or unmodeled Y.
2. Preserve RAW/WAR/WAW pairing restrictions and conservatively defer an
   overwriting instruction until the preceding result is available. This
   conservative WAW policy must still be checked against real pipeline
   behavior; it is not claimed as calibrated hardware stall time.
3. Do not let an instruction pair into an *annulled* branch-likely slot.
   Such slots consume no committed guest instruction and remain explicit
   in the observation stream; their candidate reservation policy is
   experimental.
4. Model FCR31 as a shared condition/status scoreboard resource.
   CFC1/CTC1 to FCR31, BC1F/T/FL/TL, scalar comparisons and FCSR-writing
   arithmetic need separately configured candidate latencies. FCR0 and
   other unsupported control-register operations continue to fail closed.
5. Match this project's guest decoder's actual supported COP1 comparison
   encodings (C.F.S=0x30, C.EQ.S=0x32, C.LT.S=0x34, C.LE.S=0x36).
   ADDI/DADDI remain decoder-unsupported because overflow exceptions are
   not modeled, rather than being silently priced as harmless integer ALU.
6. Preserve explicit multiply-result availability for both the GPR result
   and HI/LO. Do not assume the generic integer ALU result latency applies.
7. Give DIV.S, SQRT.S and RSQRT.S separate optional timing fields rather
   than reusing a single latency despite the manual's differing results.
8. Keep missing profile entries, unsupported instructions and HLE timing
   boundaries non-replayable. No scheduling producer is installed.

## Outstanding correctness gates

- The Table 1-3 Y combinations (e.g. COP1 operate plus COP1 move)
  require modeling the A-stage resource stall, not a guess.
- Functional-unit initiation intervals/throughput, pipeline occupancy,
  short-loop hazards, execution/retirement distinctions, cached/uncached
  access and cache-miss stalls must be modeled and validated.
- Complete COP0, VU0/COP2, MMI, FCR0, TLB/cache ops and memory-side hazard
  coverage remains absent. Treat each as a deterministic rejection until
  independently verified timing semantics exist.
- HLE entry/exit overhead and scheduler handoffs have no independently
  measured instruction/cycle trace. Do not add arbitrary costs.
- The candidate replay's synthetic regressions are **not** independent
  real-console timing traces. A trustworthy full-instruction reference
  with cycle correlation is still required before enabling the clock.
- M1-A hardware clock validation **has not passed**. M1-B first-field,
  VBlank, CSR/INTC and real boot progression **must not** be inferred
  from offline test results.

## Regressions and evidence separation

`ee_issue_timing_probe_test` covers logical pipe combinations,
branch-likely annulment and register/FCR31 ordering.
`ee_issue_trace_replay_test` covers COP1 compare/control paths,
unsupported signed traps, separate DIV/SQRT/RSQRT inputs and MULT
GPR availability. CLI and differential tests remain explicitly
synthetic until a provenance-tagged hardware/reference trace is supplied.

There are no ELF-derived timing constants or new local SDBZ boot findings
in this batch. **No GS/INTC event generation and no simulated VBlank** are
authorized by this work.
