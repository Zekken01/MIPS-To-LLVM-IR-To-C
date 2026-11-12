# VU0 / COP2 implementation research

## Decision

Keep VU0/COP2 execution unsupported in the LLVM lifter until the project models the VU0 architectural state and the EE/VU0 synchronization boundary. Do not lower QMFC2/QMTC2 as plain vector copies yet. Their register data movement is simple; the ordering and state around that movement are not.

This note records the current PCSX2 interpreter reference and the gaps in the current recompiler state model. It is a research checkpoint, not an implementation claim.

## R5900 COP2 top-level decode

The top-level COP2 opcode is `0x12`. The `rs` field selects the instruction family:

| rs | Instruction family | Behaviour |
|---:|---|---|
| `0x01` | QMFC2 | Move a 128-bit VU0 vector register to a GPR |
| `0x02` | CFC2 | Read a VU0 integer/control register into a GPR |
| `0x05` | QMTC2 | Move a 128-bit GPR to a VU0 vector register |
| `0x06` | CTC2 | Write a GPR value to a VU0 integer/control register |
| `0x08` | BC2F/T/FL/TL | VU0 condition branch; `rt` selects one of four defined table entries |
| `0x10..` | VU0 macro operation | Macro instruction groups; SPECIAL1/SPECIAL2 decode and VU pipeline semantics apply |

PCSX2's R5900 opcode table exposes QMFC2, CFC2, QMTC2, CTC2, BC2 and COP2_SPECIAL at these positions. Its COP2 SPECIAL1/SPECIAL2 dispatch is separate from the main SPECIAL and MMI tables.

The current decoder already recognises these top-level forms but marks transfer/control forms unsupported. BC2 has a known branch flow and delay slot, but remains unsupported because its condition is VU0 state that the current CPU model does not expose. The macro forms are classified by opcode tables but are not lowered.

## Transfer semantics from PCSX2's interpreter

Reference: [PCSX2 VU0.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VU0.cpp).

All four transfer/control-transfer handlers first call `vu0Sync()`. That synchronizes the VU0 micro engine with EE time; it is not equivalent to reading or writing a host-side array immediately.

There is an additional synchronization distinction when the current EE instruction is in a branch delay slot (PCSX2 checks `cpuRegs.code & 1`):

- QMFC2 and CFC2 call `_vu0FinishMicro()`: run VU0 until its E-bit termination.
- QMTC2 and CTC2 call `_vu0WaitMicro()`: run VU0 until E-bit or M-bit termination.

The synchronization helpers have materially different run policies:

- `vu0Sync()` is catch-up-only: if VU0 is active, it runs for the cycle distance `EE_cycle - VU0_cycle`. If that distance is negative, VU0 is already ahead and no VU instruction is run.
- `_vu0FinishMicro()` runs until E-bit termination, ignoring M-bit as an early stop.
- `_vu0WaitMicro()` may stop at E-bit or M-bit termination.
- The finish/wait variants add elapsed VU0 cycles to the EE, execute a VU1 catch-up block, and schedule a near-term event again if VU0 remains active.

PCSX2's `_vu0run` also checks whether VU0 is already ahead with M-bit set before entering the main loop. A host recompiler that does not model the EE cycle counter, VU0 cycle counter, E/M status and VU1 catch-up cannot faithfully replace these calls with no-ops or immediate array access.

The data movement and register-zero rules are:

### QMFC2

- Source: VU0 vector register `VF[fs]`, where `fs` is the encoded vector-register index.
- Destination: GPR `rt`.
- Copies both 64-bit halves of the 128-bit vector to the GPR.
- If `rt == 0`, no GPR write occurs.
- It performs the additional finish-micro synchronization in a delay slot.

### QMTC2

- Source: GPR `rt`.
- Destination: VU0 vector register `VF[fs]`.
- Copies both 64-bit halves.
- If `fs == 0`, no vector-register write occurs.
- It performs the additional wait-micro synchronization in a delay slot.

### CFC2

- Source index: `fs`, selecting a VU integer/control register.
- Destination: GPR `rt`; if `rt == 0`, no GPR write occurs.
- For `fs == REG_R (20)`, the returned value is `VI[R] & 0x007fffff`.
- For other indices in this handler, the 32-bit VI value is sign-extended into the low 64-bit GPR word.
- It performs the additional finish-micro synchronization in a delay slot.

### CTC2

- Source: low 32 bits of GPR `rt`.
- Destination index: `fs`; if `fs == 0`, no write occurs.
- It performs the additional wait-micro synchronization in a delay slot.
- MAC flag, TPC and VPU status are read-only in this handler.
- R is special: the write is `(value & 0x007fffff) | 0x3f800000`.
- FBRST masks the stored value and has reset/force-break side effects.
- CMSAR1 starts VU1 microcode; it is not just a VI register write.
- CLIP_FLAG updates the clip flag as well as following the current interpreter's VI write path.
- Other indices write the 32-bit VI register.

These operations therefore need a VU0-aware runtime helper or dispatcher boundary, not only a new LLVM GEP into a larger state struct.

## State required by the reference model

Reference: [PCSX2 VU.h](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VU.h).

PCSX2's VU register set contains substantially more than the 32 vector registers:

- `VF[32]`: 128-bit vector registers.
- `VI[32]`: integer/control registers, each represented in a 128-bit slot with only the low 32 bits meaningful.
- `ACC`: a separate 128-bit vector accumulator.
- `q` and `p`: divide/EFU pipeline result registers.
- VU index, cycle and flags.
- Current instruction/micro-PC state, branch-delay state, E-bit state and pending pipeline state.
- FMAC, FDIV and EFU pipeline bookkeeping and flags.

The project currently has only `m_vu0[32]` in `s_ee_cpu_state`. It has no VI/control-register bank, ACC, Q/P, VU micro-PC, VU status, branch state, cycle accounting, or pipeline state. In addition, the project's LLVM state type and native dispatcher/runtime interfaces currently assume the smaller state layout.

A direct QMFC2/QMTC2 implementation would therefore be observably wrong when a VU0 microprogram is active, and CFC2/CTC2 cannot be represented correctly with the current state.

## Implementation path

1. Define an explicit VU0 state structure with VF, VI/control registers, ACC, Q/P, status, micro-PC, E/M state and the minimum pipeline/scheduler fields required by the runtime.
2. Add runtime entry points for VU0 synchronization: sync-to-EE-cycle, finish-to-E, and wait-to-E-or-M. Keep the scheduler policy out of generated LLVM IR.
3. Implement and test the transfer semantics above through those helpers, including delay-slot variants and all zero/read-only/special-register cases.
4. Implement CFC2/CTC2 register mapping as an explicit whitelist/switch, not an unchecked array access. Treat side-effectful FBRST/CMSAR1 operations as runtime actions.
5. Add BC2 condition state and verify ordinary versus likely branch timing/annulment.
6. Only then consider VU0 macro instructions. Their dual-issue scheduling, FMAC/FDIV/EFU pipelines, flags, ACC and Q/P dependencies require independent semantic tests.

## Current guardrail

No COP2 transfer or macro opcode is promoted to `VALID` solely because its bit fields are understood. The current `UNSUPPORTED` classification is intentional until the runtime and state representation can preserve the PCSX2-observed synchronization and side effects.


## Runtime state and synchronization interface

The first runtime foundation is now represented in source, while COP2 transfer execution remains disabled:

- The existing `s_ee_cpu_state::m_vu0[32]` remains the VF vector-register bank, preserving its existing LLVM/native offsets.
- New `s_ee_vu0_state` is a 16-byte-aligned sidecar for VI[32], ACC, Q, P, MAC/clip/status/control registers, micro-PC, EE/VU cycle counters, and running/E/M state.
- `s_ee_runtime` can point to that sidecar and carry a host VU0 synchronization callback. The callback is separate from `m_user`, which remains the EE memory context.
- `ee_vu0_sync` validates its arguments and fails closed with UNAVAILABLE when either state or callback is missing. A missing scheduler is not silently treated as a completed synchronization.
- The transfer policy selector maps non-delay-slot transfers to catch-up-to-EE-cycle; delay-slot reads to finish-to-E; and delay-slot writes to wait-to-E-or-M.
- VI0 is exposed as a hardwired zero register by the new read/write helpers.

This is deliberately only an integration contract. It does not implement a VU0 execution engine or cycle scheduler, and the host callback must provide those. The current transfer opcodes remain UNSUPPORTED until a real scheduler is connected and differential transfer tests exercise both ordinary and delay-slot paths. FBRST/CMSAR1 side effects and the CFC2 special R-register mask still require explicit per-register handling.

The new `vu0_runtime_test` checks sidecar layout, VI0 behavior, all three synchronization modes, callback state identity, error propagation, and fail-closed behaviour for absent state/callback.


## Initial synchronized transfer helpers

The runtime now has first-pass implementations for QMFC2, QMTC2 and CFC2. They all call `ee_vu0_sync` before touching their destination:

- QMFC2 copies both 64-bit halves of VF[fs] into GPR[rt]. GPR0 remains unchanged, but synchronization still occurs.
- QMTC2 copies both halves of GPR[rt] into VF[fs]. GPR0 is treated as a zero source and VF0 remains unchanged; synchronization still occurs for both cases.
- CFC2 reads the VI bank after synchronization, sign-extends ordinary 32-bit VI values into the low GPR doubleword, and applies the 23-bit mask for R (VI index 20). It preserves the GPR's upper 64-bit half, consistent with the current scalar write helpers.
- All three helpers leave architectural destinations untouched when synchronization is unavailable or reports failure.

These are runtime-callable helpers with unit tests. The LLVM lifter has not yet been taught to emit them and the decoder deliberately continues to report these instructions as UNSUPPORTED. CTC2 remains deferred until its read-only, R, FBRST, CMSAR1 and CLIP_FLAG behavior is mapped and tested.

### Transfer edge cases verified against PCSX2

- VF0 is not an all-zero quadword. It is the immutable vector `(0.0f, 0.0f, 0.0f, 1.0f)`; QMFC2 must return `m_hi = 0x3f80000000000000` and a zero low half even if a backing VF0 slot contains stale data.
- QMTC2's VF0 destination is discarded.
- CFC2's R path writes only GPR `UL[0]` with the 23-bit masked value. Unlike ordinary VI reads, it does not update `UL[1]`; that pre-existing upper word must be preserved. Ordinary reads sign-extend bit 31 into `UL[1]`.
