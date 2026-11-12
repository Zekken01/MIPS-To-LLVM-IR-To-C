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
