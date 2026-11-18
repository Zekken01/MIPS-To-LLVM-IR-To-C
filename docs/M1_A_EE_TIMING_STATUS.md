# M1-A — EE timing / scheduler implementation status

**Branch:** feature/sdbz-hle-first-bootstrap  
**PR:** #142 (draft, do not merge without authorization)  
**Checkpoint:** 18 November 2026

## Actual baseline

The existing native translation path counts committed guest instruction attempts and simplified reference interpreter slots. The diagnostic issue-timing probe is explicitly experimental. Neither is calibrated R5900 cycle time. The original SDBZ US boot reached the VSync polling loop at PC 0x00175210 with no delivered fields.

The GS rational NTSC/PAL scanout, INTC latch/mask and SetVSyncFlag one-shot delivery already exist, but no validated cycle producer is wired into native execution.

## This batch

- Added a runtime-owned monotonic EE cycle timeline. It is fail-closed until an independently validated R5900 timing source is explicitly installed. It rejects decreasing timestamps and does **not** derive cycles from native dispatches, loop visits, instruction attempts, interpreter slots or host wall clock.
- Wired scheduler servicing to native dispatch boundaries. With the current unavailable source, it cannot deliver VBlank. A synthetic, explicitly enabled test clock verifies delivery at a real deadline, no early/duplicate fields, one-shot VSync registration, independent INTC/CSR behavior, monotonic timestamps and reset gating.
- Added an explicit scheduler yield diagnostic separate from cycle time.
- Added interpreted RAM execution retirement to the existing native diagnostic counters, without claiming it represents hardware cycles.
- Expanded first-boot JSON snapshots with timing source status, EE cycles, instruction slots, scheduler yields, next GS field, delivered fields, CSR, INTC status/mask and registered guest VSync addresses.
- Made SetGsCrt configure the field epoch at the supplied cycle; unsupported CRT modes no longer retain a stale NTSC/PAL schedule. It still cannot itself deliver VBlank.

## Incomplete implementation gates

M1-A is **partial**, not complete. In particular:

1. Native and interpreted instruction execution do not yet emit a calibrated unified R5900 issue/retirement timeline.
2. Pipeline stalls, HI/LO, COP0/1/2, branch/annulled-slot timing and HLE overhead require calibration against trusted reference instruction traces before hardware activation.
3. No validated timing source is installed by the game runner; the clock source is deliberately reported as \`unavailable\`. The new synthetic scheduler fixture tests scheduling semantics, **not** R5900 timing accuracy.
4. Timing-driven guest VBlank and advancement beyond PC 0x00175210 are unverified. Those are M1-B real-game validation criteria, not assumptions.

## Validation

The first scheduler commit (\`9ec3bd3\`) passed all three GitHub workflows. Subsequent diagnostics and CRT-epoch changes require the new head's CI and real-game validation. No new local SDBZ log batch has been used to claim boot progress.

Do not mark M1-A or M1-B complete until their separate acceptance gates are satisfied; do not request repeated local logs while the implementation prerequisite remains outstanding.


## Ordered issue-observation stream (next M1-A batch)

- The existing bounded `--trace-instructions=PATH` capture now represents
  native-reached instructions, RAM-interpreted commits and annulled native
  branch-likely slots in one ordered sequence, preserving per-event
  `reference_slot` and `kind`. This is suitable for comparison with
  reference execution traces when calibrating the R5900 timing model.
- Source kinds: 0 = reached in native translation; 1 = annulled delay slot;
  2 = committed by the RAM fallback. Every event includes its original
  guest PC and opcode. Annulled slots increment the slot ordinal but not
  executed instruction attempts. Unsupported RAM instructions are not
  counted or traced.
- These are **execution observations**, not calibrated issue timestamps.
  Native observations occur before execution of the lifted instruction,
  while RAM fallback records after a successful interpreter step.
  Effects must not be inferred from a native observation alone.
- This batch intentionally does **not** authorize or publish EE hardware
  cycles and cannot unblock VBlank without separately validated timing.

## Trace-driven, uncalibrated issue replay

An explicit-latency candidate consumes complete, ordered native/interpreted/annulled trace prefixes, and uses the existing two-wide issue probe to track scalar integer and load RAW dependencies across execution boundaries. Non-covered instructions, gaps, malformed delay slots and unknown latencies fail closed. This replay cannot publish hardware EE cycles or activate GS scanout. It is a calibration aid, **not** a validated R5900 timing source.

### Explicit candidate replay CLI

Run `ee_issue_trace_candidate <trace.csv> <integer-result-latency> <load-result-latency>` on an unfiltered, non-truncated trace. The analyzer fails at the first unsupported opcode, malformed/gapped reference slot or truncated stream and reports no absolute hardware time. Its output labels candidate cycles as experimental and cannot generate VBlank. A synthetic fixture and CTest cover its CSV/parser/issue path.

## Issue scoreboard resource extension

The experimental issue probe now tracks independent GPR, HI0/LO0/HI1/LO1, COP1 FPR and FPU accumulator read/write reservations. A producer may declare separate special-register/FPR/ACC latencies from its GPR latency. Validation is atomic, so invalid masks and overflow cannot partially mutate a timing state. Dedicated tests exercise scalar HI/LO multiply-to-MFLO, FPR producer/consumer, accumulator and same-cycle pairing dependencies. No new hardware-clock source is installed; these latencies remain caller supplied and uncalibrated.

### Explicit HI/LO timing categories

The offline issue candidate now classifies scalar `MULT/MULTU/DIV/DIVU`, `MFHI/MFLO/MTHI/MTLO` and their second-pipeline `*1` equivalents, plus `MADD/MADDU/MADD1/MADDU1`, using separate HI/LO read/write dependency masks. Their four latency categories default to **unconfigured**: multiply, divide, HI/LO transfer and accumulate. Existing candidate traces remain backward compatible. The CLI accepts the four optional latency values as one group; these values are diagnostic inputs, not hardware validation. Other MMI operations remain unmodeled and fail closed.

### Scalar COP1 issue classes

The offline candidate can now replay supported MFC1/MTC1, COP1.S scalar arithmetic, DIV/SQRT/RSQRT, ACC operations and CVT.S.W when **four separate caller-provided COP1 latencies** are available: transfer, arithmetic, divide/sqrt and accumulate. FPR0 is modeled as writable, independently from GPR0. Unmodeled FCSR/FCC0, COP0, VU0 and packed-MMI hazards still fail closed, so these cases cannot be used to authorize GS timing. CTests exercise FPR and ACC cross-instruction dependency sequencing.

### Explicit host HLE entry boundaries

Native and RAM instructions, annulled slots and recognized EE startup/registered syscall HLE handoffs now share one bounded observation stream. `kind=3` identifies an HLE boundary, carries `hle_service`, and records the *current* instruction/slot ordinals without consuming any new slot or time. The runner also exposes a cumulative `hle_timing_boundaries` diagnostic. The offline replay rejects HLE boundaries as unmodeled rather than silently assigning guessed kernel time. This is not a calibrated HLE cost and does not enable GS/INTC events.

### Per-instruction candidate issue-cycle output

The offline analyzer now optionally writes `--candidate-events=<path>` after all profile arguments, producing `reference_slot,pc,opcode_word,kind,candidate_issue_cycle`. Each event is stamped from the candidate probe's own issue/reservation state, not from a host dispatch or guest instruction count. The CSV is created only for a complete, supported, non-truncated trace; replay failures produce no candidate timeline. These are experimental relative cycles and cannot drive VBlank.
