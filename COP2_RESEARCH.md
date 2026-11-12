# VU0 / COP2 implementation research

## Decision

The decoder and LLVM lifter now support the COP2 transfer/control forms QMFC2, QMTC2, CFC2, CTC2 and the four BC2 branches through runtime helpers. They do **not** provide a VU0 micro-engine. A host must supply the VU0 state, execution callback, VU1 catch-up callback and event scheduling hook; absent services fail closed.

VU0 macro instructions remain unsupported. Do not promote SPECIAL1/SPECIAL2 forms until a real micro-engine and the required FMAC/FDIV/EFU pipeline semantics are available.

This note records the PCSX2 interpreter reference, implemented integration boundary and remaining semantic gaps. The runtime helper tests are not proof of a complete VU0 execution engine.

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

The decoder now classifies the four transfer forms and BC2F/T/FL/TL as VALID only for their documented encodings. The LLVM path invokes the synchronized runtime helpers; BC2 reads VPU_STAT bit 8 and retains ordinary/likely delay-slot behaviour. If the runtime has no VU0 sidecar/scheduler where required, generated code exits at the guest instruction instead of guessing.

The VU0 SPECIAL1/SPECIAL2 macro forms are still classified by opcode table but remain unsupported by the lifter.

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

The project retains `s_ee_cpu_state::m_vu0[32]` as the VF bank and adds a separate aligned `s_ee_vu0_state` sidecar. The sidecar provides VI[32], ACC, Q/P, visible control/flag values, micro-PC, EE/VU cycle counters, E/M/running state, and separate 4 KB VU data and micro-program memories without shifting the established CPU-state offsets.

The sidecar is not a full `VURegs` equivalent. It does not contain a complete pipeline scoreboard, branch-delay bookkeeping, FMAC/FDIV/EFU internal state, or a VU1 execution engine. Those remain host-engine responsibilities. A helper-only mock can validate ABI, fetch and transfer rules, but cannot establish correct behaviour while a real VU microprogram is executing.

## Implementation path

1. **Host VU0 engine:** provide the execution callback that consumes bounded cycle budgets, updates VF/VI/ACC/Q/P, E/M status and micro-PC, and preserves the VU pipeline state.
2. **Scheduler integration:** install `ee_vu0_scheduler_sync_callback` as the runtime sync callback and supply the VU1 catch-up and EE event callbacks. Differential-test the three scheduling policies against PCSX2.
3. **Control actions:** implement FBRST reset/force-break and CMSAR1 VU1 startup in the host control callback. The current runtime only routes these actions; it does not emulate them.
4. **Runtime validation:** test real microprograms that write VI/VF registers while the EE executes transfers in ordinary code and delay slots.
5. **VU0 macro instructions:** only after the engine has correct dual-issue scheduling, FMAC/FDIV/EFU pipelines, flags, ACC/Q/P forwarding and branch timing.

## Current guardrail

QMFC2/QMTC2/CFC2/CTC2 and BC2 are enabled only through the fail-closed runtime boundary and only when the required host callbacks/state exist. This is integration support, not a claim that the host contains a VU0 engine. All VU0 macro instructions remain unsupported.


## Runtime state and synchronization interface

The runtime sidecar and COP2 integration are now represented in source:

- The existing `s_ee_cpu_state::m_vu0[32]` remains the VF vector-register bank, preserving its existing LLVM/native offsets.
- The 16-byte-aligned `s_ee_vu0_state` contains VI[32], ACC, Q/P, MAC/clip/status/control registers, TPC/micro-PC, EE/VU cycle counters, E/M/running state, and separate 4 KB data and micro-program memories.
- `m_tpc` is the visible VI[TPC] value in 8-byte instruction slots; `m_micro_pc` is the byte offset used by the active fetch loop.
- `ee_vu0_fetch_micro_instruction` fetches an aligned pair of little-endian 32-bit lower/upper words, decodes upper I/E/M/D/T flags, advances the byte PC by eight, and wraps at the 4 KB program boundary. Invalid/misaligned fetches leave both the PC and output untouched.
- `s_ee_runtime` can point to that sidecar and carry a host VU0 synchronization callback. The callback is separate from `m_user`, which remains the EE memory context.
- `ee_vu0_sync` validates its arguments and fails closed with UNAVAILABLE when either state or callback is missing. A missing scheduler is not silently treated as a completed synchronization.
- The transfer policy selector maps non-delay-slot transfers to catch-up-to-EE-cycle; delay-slot reads to finish-to-E; and delay-slot writes to wait-to-E-or-M.
- VI0 is exposed as a hardwired zero register by the new read/write helpers.

This remains a fetch and scheduling foundation, not a VU0 interpreter. The host engine must still decode and execute upper/lower operations, resolve dual-issue dependencies, maintain delayed writes and FMAC/FDIV/EFU pipelines, process branches and end flags, and update architectural state. Transfer forms are integrated into the LLVM path and remain guarded by missing/failed scheduler callbacks.

The `vu0_runtime_test` suite checks sidecar layout, VI0 behavior, micro fetch and wrap, flag decoding, transfer helpers, synchronization modes, callback state identity, error propagation, and fail-closed behaviour. Its mock-engine cases validate orchestration, not instruction semantics.


## Initial synchronized transfer helpers

The runtime now has first-pass implementations for QMFC2, QMTC2 and CFC2. They all call `ee_vu0_sync` before touching their destination:

- QMFC2 copies both 64-bit halves of VF[fs] into GPR[rt]. GPR0 remains unchanged, but synchronization still occurs.
- QMTC2 copies both halves of GPR[rt] into VF[fs]. GPR0 is treated as a zero source and VF0 remains unchanged; synchronization still occurs for both cases.
- CFC2 reads the VI bank after synchronization, sign-extends ordinary 32-bit VI values into the low GPR doubleword, and applies the 23-bit mask for R (VI index 20). It preserves the GPR's upper 64-bit half, consistent with the current scalar write helpers.
- All three helpers leave architectural destinations untouched when synchronization is unavailable or reports failure.

These runtime-callable helpers are now emitted by the LLVM lifter for QMFC2, QMTC2, CFC2 and CTC2, and the decoder validates their reserved bits before classifying them as VALID. CTC2's read-only, R, FBRST, CMSAR1 and CLIP_FLAG cases are handled explicitly; side-effectful FBRST/CMSAR1 operations still depend on a host control callback. JIT integration tests exercise transfers, cache barriers, failure exits and delay-slot synchronization.

### Transfer edge cases verified against PCSX2

- VF0 is not an all-zero quadword. It is the immutable vector `(0.0f, 0.0f, 0.0f, 1.0f)`; QMFC2 must return `m_hi = 0x3f80000000000000` and a zero low half even if a backing VF0 slot contains stale data.
- QMTC2's VF0 destination is discarded.
- CFC2's R path writes only GPR `UL[0]` with the 23-bit masked value. Unlike ordinary VI reads, it does not update `UL[1]`; that pre-existing upper word must be preserved. Ordinary reads sign-extend bit 31 into `UL[1]`.


## CTC2 control-register handling

The first CTC2 runtime helper now implements the interpreter's explicit register cases:

- VI0 writes are ignored; ordinary writable registers update VI.
- MAC_FLAG, TPC and VPU_STAT are read-only.
- R stores `(value & 0x007fffff) | 0x3f800000`.
- CLIP_FLAG updates both the VI slot and the separately tracked clip flag.
- FBRST stores `value & 0x0c0c`; when any reset/break action bits are present, a host control callback must handle them.
- CMSAR1 is routed through the host control callback, which is responsible for finishing/starting the VU1 micro subroutine using the low 16-bit address.
- Missing/failed host control callbacks fail closed before the helper commits the relevant register state.

The helper does not emulate VU1 or execute FBRST hardware actions itself. These remain responsibilities of the host control callback. QMFC2/QMTC2/CFC2/CTC2 and BC2F/T/FL/TL are now decoded and lowered through the runtime boundary; VU0 SPECIAL1/SPECIAL2 macro instructions remain UNSUPPORTED.
## Bounded scheduler policy adapter

`ee_vu0_scheduler_sync_callback` now implements the orchestration around a host-provided micro-engine slice callback:

- Catch-up-only calls the engine with the exact positive difference between EE and VU0 cycle counts; when VU0 is already ahead it does no execution. It does not account cycles on the EE, catch VU1 up, or schedule an EE event.
- Finish-to-E repeatedly runs with STOP_AT_E and ignores M-bit termination.
- Wait-to-E-or-M runs with STOP_AT_E and STOP_AT_M. The early M-bit/already-ahead path advances EE time to the VU0 cycle and returns without the normal epilogue.
- Finish/wait account the elapsed VU0 cycles on EE, call the host VU1 catch-up hook, and schedule a four-cycle EE event if VU0 remains active.
- Every callback result is validated. Missing VU1/event services are detected before interlocked execution; non-progressing engines and excessive run-call counts fail rather than looping indefinitely.

The adapter deliberately does not execute VU micro-instructions. The host's `m_execute` callback is responsible for instruction semantics, pipeline hazards, micro-PC advancement and updating the sidecar's cycle/E/M state. Tests use a mock engine to validate scheduler orchestration only.
