# Work Log

## 2026-11-11 — Recovery baseline

- Confirmed private GitHub repository access and created this dedicated recovery branch from the initial repository README.
- Recovered the latest source follow-up preserved in the Library (09:57 UTC).
- Inspected the separately named 14:30 worktree archive; despite its name it contains the older reviewed source and does not contain the newer four-test state.
- Reconstructed the source from the 09:57 follow-up over the reviewed baseline.
- Performed a clean CMake/Ninja build and ran CTest: 4/4 passing; decoder regressions: 269/269.
- The exact later 2/4 worktree and logs remain unrecovered because the previous chat never exported them.

Further changes should be committed to this branch in small, testable increments. Do not merge it into `main` until the source and test baseline have been reviewed.


## 2026-11-11 — MMI arithmetic and MAC pipeline follow-up

- Made CI install `llvmlite` in an isolated virtual environment so the LLVM JIT runtime test is included rather than silently skipped.
- Expanded the O0–O3 JIT fixture to check the exact enabled MMI0 arithmetic operations (PADD/PSUB/PMAX at word, halfword, and byte widths), alongside PAND/PXOR/POR/PNOR, HI1/LO1 transfers, aliasing, upper-GPR preservation, and cross-block writeback.
- Corrected MADD/MADDU and MADD1/MADDU1. They now accumulate the signed/unsigned 32×32 product into the appropriate 64-bit HI:LO or HI1:LO1 accumulator, write sign-extended result words back to the special registers, and return the low result word in rd.
- Updated decoder validity and lifter mapping for the four exact MADD/MADDU encodings; nonzero reserved `sa` forms remain unsupported.
- Verified against the EE Core Instruction Set Manual, Version 6.0, MADD/MADD1/MADDU/MADDU1 descriptions and operation pseudocode.
- GitHub Actions run [43089047322](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43089047322): clean configure/build and **4/4 CTest passed**, including the LLVM JIT runtime fixture.



## 2026-11-11 — Pipeline-1 multiply support

- Added exact decoder/lifter coverage for `MULT1` and `MULTU1` (funct 0x18/0x19, reserved `sa` must be zero).
- Implemented signed and unsigned 32×32 multiplication into HI1/LO1, including sign-extension of the resulting 32-bit HI1/LO1 words and the low-word result in rd.
- Extended the LLVM JIT fixture to verify MULT1/MULTU1, in addition to MADD/MADDU and the MMI arithmetic/logical operations.
- Verified against the EE Core Instruction Set Manual v6.0, MULT1 and MULTU1 operation definitions.
- GitHub Actions run [43089471611](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43089471611): clean configure/build and **4/4 CTest passed**, including the JIT runtime fixture at O0–O3.


## 2026-11-11 — Pipeline-1 division support

- Added exact decoder/lifter support for `DIV1` and `DIVU1` (funct 0x1a/0x1b; reserved `sa` must be zero).
- Implemented signed and unsigned 32-bit quotient/remainder lowering into LO1/HI1, respectively.
- Guarded signed `INT_MIN / -1` before LLVM `sdiv`/`srem`, then applied the EE-defined quotient/remainder result.
- The EE manual leaves divide-by-zero results undefined. The recompiler chooses a deterministic no-op for HI1/LO1 on a zero divisor, avoiding LLVM undefined behavior/poison.
- Added an independent JIT fixture covering signed normal division, signed overflow, signed divide-by-zero, unsigned division, and unsigned divide-by-zero; each case also verifies MFHI1/MFLO1 readback at O0–O3.
- GitHub Actions run [43090955314](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43090955314): clean configure/build and **5/5 CTest passed**.

## 2026-11-11 — Exact MMI1 packed operations

- Added exact decoder/lifter/LLVM lowering for 13 MMI1 operations:
  - `PABSW` and `PABSH`, including the defined clamp of signed minimum to signed maximum.
  - `PCEQW`, `PCEQH`, and `PCEQB`.
  - Signed `PMINW` and `PMINH`.
  - Unsigned saturating `PADDUW/H/B` and `PSUBUW/H/B`.
- Restricted unary absolute-value forms to their documented `rs=0` encodings.
- Added a separate O0–O3 LLVM JIT fixture that checks all 13 operations, lane ordering, saturation edges, and a destination/source alias.
- Updated the decoder regression for the game entry-point word `0x70000c28`: it is now correctly accepted as exact `PADDUW`, not rejected.
- Left the remaining MMI1 operations (`PADSBH`, `PEXTUW/UH/UB`, and `QFSRV`) unsupported pending their own exact implementations and tests.
- GitHub Actions run [43093331006](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43093331006): clean configure/build and **6/6 CTest passed**.

## 2026-11-11 — Complete the MMI1 operation table

- Extended the exact MMI1 implementation to cover the remaining defined sub-ops: `PADSBH`, `PEXTUW`, `PEXTUH`, `PEXTUB`, and `QFSRV`.
- Added exact `MTSAB` and `MTSAH` lowering so QFSRV obtains its byte/halfword shift amount from the architectural SA field rather than a test-only preset.
- Added the three-instruction SA read-hazard gap before the second SA-setting instruction in the runtime fixture.
- Extended decoder/lifter assertions and the LLVM JIT fixture to exercise all 18 non-reserved MMI1 sub-ops, plus MTSAB/MTSAH, at O0–O3 and multiple SA values.
- The source and test changes are committed on this branch. CI runs for the latest commits are currently queued; no final result is recorded yet.

## 2026-11-11 — Host dispatcher and cross-function control-flow validation

- Added end-to-end LLVM JIT coverage for dynamic `JALR` across separately translated caller and callee functions. The test checks that the target register is sampled before the delay slot overwrites it, `rd` receives the PC+8 link, the callee returns, and the caller resumes at its PC+8 continuation.
- Added `ee_dispatch_run()` as a bounded host dispatcher over `s_recompiled_elf::functions` plus a parallel array of native entry points.
- The dispatcher resolves the current guest PC to its owning translated function. Both `EE_EXIT_RETURN` and `EE_EXIT_DISPATCH` re-enter translated code when the next PC belongs to a registered block; an unmapped PC is returned to the outer host with its original exit reason.
- Added explicit invalid-table checks and a maximum-dispatch budget so a malformed guest loop cannot monopolise the host indefinitely.
- Added six native dispatcher tests: direct caller/callee/return continuation, dynamic JALR/callee/caller continuation, return to external code, unknown initial PC, dispatch-budget exhaustion, and metadata/native-entry array mismatch.
- Moved `find_recompiled_function()` into the core library so dispatcher clients and tests share the same implementation without linking the full ELF recompiler into the decoder test.
- GitHub Actions run [43109193936](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43109193936): clean configure/build and **9/9 CTest passed**, including the new `dispatcher` test and all LLVM JIT suites.
- Limitation: the dispatcher is now implemented and independently tested, but the real LLVM object emitter still returns false. The dispatcher therefore has not yet been wired to a complete set of generated native game functions.

## 2026-11-11 — Real-game exception continuation and full-image validation

- Ran the CI-built `ee_recomp` binary against the preserved USA `SLUS_214.42` ELF.
- The first run emitted only the initial bootstrap function. Inspection showed that startup code executes `SYSCALL` and then continues after the exception handler; static discovery treated `FLOW_EXCEPTION` as an unconditional end and never reached the later direct JAL targets.
- Updated `collect_successors()` to include the possible `EPC+4` resume PC for exception instructions. Updated the CFG builder to retain that continuation as a separate dispatchable block while keeping the syscall/break block itself as a runtime exception exit.
- Added regressions for successor discovery after both SYSCALL and BREAK, plus CFG retention of code after both exception blocks.
- Re-ran the CI-built recompiler on `SLUS_214.42` (entry `0x00100008`, machine 8, two segments): **7,849 functions discovered and emitted**, covering **417,738 unique instruction PCs**. The current lifter reports 409,530 supported and 8,208 unsupported instructions (98.0%).
- Parsed and LLVM-verified all **7,849/7,849** generated modules with LLVM 20.1.8; zero invalid modules.
- Also ran llvmlite native target code generation at `opt=0` over all **7,849/7,849** modules. All succeeded, producing **34,220,040 bytes** of aggregate in-memory host object code.
- The validation used the Linux `ee_recomp` and `verify_ir.py` from CI run [43110647060](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43110647060), commit `d76f7ec`.
- Limitation: llvmlite emitted host objects in memory for validation; the project `object_emitter::emit_object()` remains a stub, and these objects were not linked into a runnable game executable.

## 2026-11-11 — Enable exact 64-bit scalar comparisons

- Confirmed the existing LLVM emitter already contains exact lowerings for `SLT`, `SLTU`, `SLTI`, and `SLTIU`, and the CFG liveness analysis already accounts for their source and destination registers. The decoder was still marking those operations unsupported.
- Marked `SLTI` and `SLTIU` valid, and marked `SLT`/`SLTU` valid only when their reserved shift-amount field is zero. Non-zero reserved encodings remain unsupported.
- Added decoder coverage and a generated LLVM JIT fixture checking signed versus unsigned 64-bit ordering, sign extension of the `SLTIU` immediate, read/write aliases, destination `$zero`, and preservation of the upper 64-bit GPR halves.
- Fixed the JIT harness to register the native LLVM target and assert the expected sequential dispatcher exit/PC.
- GitHub Actions run [43112984141](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43112984141), commit `5aba8e1`: clean configure/build and **10/10 CTest passed**.
- Re-ran the CI-built recompiler against USA `SLUS_214.42`: still **7,849 functions / 417,738 PCs**, with **414,916 supported** and **2,822 unsupported** instructions (**99.3%**, up from 98.0%). The 5,386 newly supported instructions are exactly the four scalar comparison families.
- LLVM parse/verify: **7,849/7,849 passed**, zero invalid modules.
- Native host-object generation via llvmlite at `opt=0`: **7,849/7,849 passed**, total **34,756,008 bytes** of in-memory object code (30,660,224 bytes for the first 7,000 modules plus 4,095,784 bytes for the remaining 849).
- The project's own `object_emitter::emit_object()` remains a stub; object bytes were emitted in-memory by llvmlite for validation and were not linked into a runnable executable.
## 2026-11-11 — COP1 FCC0 comparisons and full-game native linking

- Added exact FCC0-only `C.F.S`, `C.EQ.S`, `C.LT.S`, and `C.LE.S` decoding/lowering. Other condition-code destinations remain unsupported because the current state model only exposes FCC0.
- Added LLVM JIT coverage for all four predicates, signed zero, denormal flushing, infinity/NaN saturation to signed FMAX, and preservation of non-FCC0 FCSR bits.
- Corrected the fixture to match the R5900 comparison operand normalisation rather than applying host IEEE unordered-NaN expectations directly.
- GitHub Actions run [43115526527](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43115526527): **12/12 CTest passed**.
- Extended `tools/link_native_objects.py` to compile its large generated dispatcher metadata translation unit separately at a configurable optimisation level (default O0). Fixed response-file quoting/newline emission for thousands of object inputs.
- GitHub Actions run [43117522952](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43117522952), commit `f583796`: clean build and **12/12 CTest passed**, including the object-emission/link/dispatcher integration test.
- Recompiled the preserved USA `SLUS_214.42` ELF with the matching CI build: **7,849 functions**, **417,738 unique instruction PCs**, **416,648 supported / 1,090 unsupported** (99.7% decoder/lifter classification).
- LLVM 20.1.8 parse/verify and native object generation at O0 passed for **7,849/7,849 modules**. The persisted objects total **35,026,424 bytes** and all 7,849 defined guest symbols were checked against the generated metadata.
- Ran the committed linker tool over all 7,849 objects and the matching runtime/dispatcher sources. It produced a **28,618,808-byte** x86-64 shared library exporting `ee_game_run` and `ee_fn_00100008`. The library loaded through `ctypes.CDLL`; `ldd -r` reported no unresolved dynamic symbols.
- Full validation details: `FULL_GAME_NATIVE_LINK_VALIDATION.md`.
- Limitation: the linked library is not a bootable PS2 game and the full game was not executed. The EE runtime still lacks BIOS/IOP, GS, DMA, TLB/MMIO devices, interrupts, and other console services. The in-tree `object_emitter::emit_object()` remains a stub; the persisted objects were emitted using the external llvmlite validation tool.


## 2026-11-11 — CVT.S.W JIT regression coverage

- Extended the existing COP1 LLVM fixture to emit the exact `fmt=W, funct=CVT.S.W` encoding.
- Added O0 JIT runtime cases for zero, positive/negative values, the 16,777,217 single-precision rounding boundary, and signed 32-bit extrema.
- The test verifies the low 32-bit signed source is used, the destination is written as a zero-extended FPR word, the source is preserved, and FCSR is unchanged.
- The lowering itself was already present (`sitofp i32 to float`); this change adds execution coverage for the only supported `fmt=W` operation.
- CI result for this follow-up is pending; do not treat it as verified until the Actions run completes.


## 2026-11-12 — Complete supported COP1-S coverage and record COP2 boundary

- Compared the COP1 decoder/lifter coverage against PCSX2's R5900 COP1 table. Kept non-table conversion forms unsupported; the R5900 table does not define the conventional ROUND.W.S/TRUNC.W.S/CEIL.W.S/FLOOR.W.S family.
- Corrected MAX.S/MIN.S to use the EE's raw signed-bit ordering instead of host floating-point comparisons. Added raw-bit ABS.S/NEG.S handling and exact FCSR cause-bit clearing.
- Added EE operand normalisation and result handling for ADD.S/SUB.S/MUL.S/DIV.S and SQRT.S/RSQRT.S, including denormal flushing, signed FMAX saturation, divide-by-zero classification, and cause/sticky flag behavior.
- Added distinct accumulator paths for ADDA.S/SUBA.S/MULA.S, MADD.S/MSUB.S and MADDA.S/MSUBA.S. Runtime tests distinguish the normalized accumulator/product path from the raw accumulator/product path using denormal and overflow cases.
- Added MFC1/MTC1/CFC1/CTC1 runtime tests. Fixed block read/write liveness so CFC1 marks its destination GPR as written and CTC1 marks its source GPR as read. A cross-block ADDIU-to-CTC1 test confirms the value reaches FCSR.
- Added BC1T condition-sampling coverage with a CTC1 delay slot, plus BC1FL not-taken delay-slot annulment coverage.
- The first BC1 JIT test attempt failed only because the Python test called the generated two-argument guest ABI with one argument. Passed the runtime pointer and reran.
- GitHub Actions run [43125195580](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43125195580), commit `ce5f3522`: clean configure/build and **12/12 CTest passed**, including all COP1 JIT tests.
- Added `COP2_RESEARCH.md` using PCSX2's VU0 interpreter and state structures as references. Documented QMFC2/QMTC2/CFC2/CTC2 synchronization and side effects, BC2/macro decode groups, and the current project state gap. No COP2 opcode was promoted to VALID; VU0 transfers require a scheduler/state model rather than a direct register-copy lowering.


## 2026-11-12 — COP2 transfer encoding validation

- Started the VU0 transfer target without prematurely enabling execution. QMFC2, CFC2, QMTC2 and CTC2 remain `UNSUPPORTED` because their EE/VU0 synchronization and register side effects are not implemented.
- Tightened COP2 decoding: all four transfer encodings require the low eleven `sa`/`funct` bits to be zero. Reserved-bit violations now classify as `INVALID`, rather than being conflated with valid-but-unimplemented transfers.
- Added positive and negative decoder cases for all four transfer forms.
- GitHub Actions runs [43170109202](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43170109202) and [43170132717](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43170132717) were queued/in progress at the time of this note; verify their final results before treating this change as tested.
- Next: build the VU0 runtime state and synchronization interface, then test the transfer helpers independently before allowing the lifter to emit them.

- CI caught a mnemonic-table index error in the first patch (the compiler build passed; the decoder test reported two failures). Corrected the selector-to-name mapping in commit `f8762ed`; that commit's Actions result is pending.


## 2026-11-12 — VU0 runtime state and scheduler boundary

- Added a separately aligned `s_ee_vu0_state` sidecar for the VU0 VI bank, ACC/Q/P, visible flags/control registers, micro-PC, EE/VU cycle counters and E/M/running state. Kept the existing VF vector bank in `s_ee_cpu_state::m_vu0[32]` to avoid shifting its established LLVM/native ABI offsets.
- Extended `s_ee_runtime` with an optional sidecar pointer and a dedicated VU0 scheduler callback/context; the existing `m_user` memory context remains first and unchanged.
- Added `ee_vu0_sync`, a fail-closed callback boundary. Missing VU0 state or scheduler returns UNAVAILABLE rather than pretending synchronization succeeded.
- Added the transfer synchronization policy selector: ordinary transfers catch up to the EE cycle; delay-slot reads finish to E; delay-slot writes wait to E or M.
- Added VI read/write helpers with VI0 hardwired to zero.
- Added `vu0_runtime_test` coverage for layout, VI0, all synchronization modes, callback state/PC propagation, callback failures, and missing-runtime fail-closed paths.
- This does not yet provide an actual VU0 scheduler. COP2 transfer decodes remain UNSUPPORTED until a host scheduler is connected and tested against PCSX2 timing semantics.


## 2026-11-12 — First synchronized COP2 transfer helpers

- Added runtime helpers for QMFC2, QMTC2 and CFC2. Each calls the VU0 synchronization callback before performing any architectural transfer; missing or failed synchronization leaves the transfer destination unchanged.
- QMFC2 copies the full 128-bit VF value to the full 128-bit GPR, respecting hardwired GPR0.
- QMTC2 copies the full 128-bit GPR value to VF, treats GPR0 as a zero source, and protects VF0 from writes.
- CFC2 sign-extends ordinary VI values into the low GPR doubleword, masks the R register (VI index 20) to 23 bits, and preserves the GPR upper 64-bit half.
- Expanded `vu0_runtime_test` to cover full-width copies, zero registers, CFC2 R handling, scheduler failures and destination preservation.
- The LLVM emitter and decoder are intentionally unchanged: these transfer helpers are not yet wired to generated guest code, and the corresponding instructions remain UNSUPPORTED. CTC2 is deferred pending explicit side-effect mapping.

- Follow-up semantic review against PCSX2 corrected two edge cases in the transfer helpers: QMFC2 reads VF0 as the constant (0,0,0,1.0f), and CFC2's R-register path replaces only GPR UL[0] while preserving UL[1].


## 2026-11-12 — CTC2 special-register runtime semantics

- Added a dedicated host control-action callback for CTC2 FBRST and CMSAR1. It is separate from the VU0 cycle-sync callback.
- Implemented VI0 write suppression, ordinary VI writes, read-only MAC_FLAG/TPC/VPU_STAT, the R-register exponent rule, CLIP_FLAG mirroring, FBRST masking and CMSAR1 low-16-bit dispatch.
- CFC2 now reads the dedicated tracked MAC/clip/status/TPC/FBRST/VPU_STAT values where those indices map to PCSX2's visible special-register state.
- Side-effectful FBRST/CMSAR1 writes fail closed when the host action callback is missing or fails; relevant register state is not committed on failure.
- Expanded `vu0_runtime_test` for all of those cases. COP2 decoder and LLVM emission remain disabled pending JIT integration.


## 2026-11-12 — Bounded VU0 scheduler adapter and COP2 integration status

- Added a host scheduler interface with an execution-slice callback, a VU1 catch-up callback, and an EE event-scheduling callback. The host micro-engine remains responsible for executing VU instructions and updating VF/VI/ACC/Q/P, micro-PC, cycle counters, and E/M status.
- Implemented `ee_vu0_scheduler_sync_callback` as the reusable orchestration layer:
  - Catch-up-only uses the exact positive EE/VU0 cycle difference, does not run when VU0 is already ahead, and does not account EE cycles or invoke VU1/event callbacks.
  - Finish-to-E ignores M-bit termination; wait-to-E-or-M requests both stop conditions and includes the already-ahead M-bit fast path.
  - Normal interlocked completion accounts the elapsed VU0 cycles on EE, catches VU1 up, and schedules a four-cycle EE event if VU0 remains active.
  - Missing callbacks, host failures, invalid results, non-progressing engines and execution-call budget exhaustion fail closed.
- Added mock-engine regression coverage for exact catch-up budget, VU0-ahead behaviour, finish/wait termination, M-bit fast path, VU1 catch-up, event scheduling, callback failures and execution limits.
- Updated `COP2_RESEARCH.md` and `RESUME.md` to reflect the later COP2 transfer/BC2 LLVM integration and to distinguish the scheduler adapter from a real VU0 micro-engine.
- GitHub Actions [run 43180405182](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43180405182): clean configure/build and **14/14 CTest passed**, including `vu0_runtime` and `vu0_transfer_ir_runtime` plus the existing suites.
- Documentation follow-up Actions [run 43180563505](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43180563505) also completed successfully.
- Remaining: supply a real host VU0 micro-engine and differential-test real running microprograms against PCSX2. VU0 SPECIAL1/SPECIAL2 macros remain UNSUPPORTED.


## 2026-11-12 — Correct VU0 interlocked EE cycle accounting

- Cross-checked the scheduler adapter against PCSX2 `VU0.cpp` `_vu0run`: the normal finish/wait epilogue advances EE cycle by the amount of VU0 execution consumed during that wait (`VU0.cycle - startcycle`); it does not assign EE cycle to the final VU0 cycle.
- Corrected `ee_vu0_scheduler_sync_callback` to preserve both start counters and add only the elapsed VU0 cycles to the original EE cycle. The special early M-bit/already-ahead path remains an assignment to the VU0 cycle, matching PCSX2's separate early return.
- Strengthened tests so finish-to-E starts with VU0 already ahead of EE, and wait-to-E-or-M verifies EE advances by the consumed amount while remaining offset from VU0.
- GitHub Actions [run 43182310781](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43182310781): clean configure/build and **14/14 CTest passed**.
- Next micro-engine work remains gated on the 4 KB VU0 micro-memory, paired 64-bit instruction format, upper/lower dual-issue ordering, delay/branch/E/M state, and pipeline hazards. Implementing isolated lower-ALU operations without those rules would not be a safe execution engine.


## 2026-11-12 — VU0 local memories and paired micro fetch

- Added the two architectural 4 KB VU0 memories as separately 16-byte-aligned arrays in the VU0 sidecar: data memory and micro-program memory. Existing CPU-state layout and offsets remain unchanged because the sidecar is separate and the arrays are appended after its existing fields.
- Added `ee_vu0_fetch_micro_instruction`: fetches the paired lower/upper 32-bit words from the byte-addressed micro store, explicitly decodes little-endian words, extracts the upper I/E/M/D/T flag bits, advances by eight bytes and wraps at `0x1000`.
- Invalid or unaligned fetches fail without changing the PC or output record. `m_tpc` is documented in instruction-slot units; `m_micro_pc` is the active byte fetch address.
- Added compile-time state layout checks plus runtime tests for pair ordering, all five control flags, PC increment, end-of-memory wrap, invalid alignment and null arguments.
- Expanded the Python JIT harness's `VU0State` mirror with the two memories and explicit 16-byte padding, including offset/size assertions (`data_memory=240`, `micro_memory=4336`, `sizeof=8432`).
- GitHub Actions [run 43183141780](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43183141780): clean configure/build and **14/14 CTest passed**.
- This is fetch/storage only. The VU0 dual-issue interpreter, instruction decoder, delayed VI/VF writes, branch pipeline, and FMAC/FDIV/EFU hazard machinery remain unimplemented. Macro instructions remain unsupported.


## 2026-11-12 — VU0 micro-op primary and secondary decode tables

- Added symbolic decode identities for the documented VU0 lower and upper micro-op table entries. Each fetched instruction now carries the decoded operation for its lower and upper word, alongside the raw primary opcode, FT/FS/FD, IT/IS/ID and XYZW fields.
- Lower primary decode uses `word >> 25`. Primary `0x3c` is the `LowerOP` dispatch; the low-six-bit secondary selector maps IADD/ISUB/IADDI/IAND/IOR and `0x3c..0x3f` to the four T3 subtables indexed by bits 10..6.
- Upper primary decode uses `word & 0x3f`. Upper primaries `0x3c..0x3f` select the four FD subtables indexed by bits 10..6. Other upper primary slots are decoded directly.
- Unassigned primary/secondary slots map to `UNKNOWN`; no micro-op execution has been enabled. This prevents an encoding being mistaken for an implemented operation.
- A first attempt misread the lower secondary-table selector as primary `0x60`; table recounting against PCSX2 `VUops.cpp` showed the correct primary is `0x3c`. Corrected implementation and test encodings. Tests also corrected sample-word expectations (`0x11223344` lower = IADDIU; `0xf8001234` upper = unassigned primary `0x34`). Added a separate upper ADDi (`0x22`) vector.
- GitHub Actions [run 43185095496](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43185095496): clean configure/build and **14/14 CTest passed**, covering the corrected lower IADD/T3 LQI paths, upper ADDx/ADDi/FD-NOP and unassigned table slots.
- Remaining: implement issue/retirement scheduling with delayed VI/VF writes, branch-delay state and the IALU/FMAC/FDIV/EFU pipeline interlocks before executing any nontrivial VU instruction pair.


## 2026-11-12 — First executable VU0 lower-IALU micro-op slice

- Added `ee_vu0_execute_ialu_callback` behind the existing scheduler execution callback ABI. The first slice accepts only lower IADD, ISUB, IAND, IOR, IADDI, IADDIU and ISUBIU paired with upper NOP. Valid but unsupported pairs return UNAVAILABLE and restore the micro-PC to the pair start.
- VI operations read/write the low 16-bit halfword; writes preserve the neighbouring upper halfword and VI0 remains hardwired. Immediate widths/sign rules and destination fields are exercised by runtime tests.
- Integrated the existing four-entry IALU reservation queue: execution retires matured entries before issuing an operation, schedules one-cycle destination reservations, and flushes pending IALU reservations when E terminates the microprogram.
- E flag uses the two-pair delay observed in PCSX2. M flag ends each engine execution slice independently of the scheduler's wait policy. FINISH_TO_E resumes with another slice after M; WAIT_TO_E_OR_M returns at M. The scheduler integration test executes an M-marked pair, resumes, then completes an E-marked pair plus its delay instruction.
- PCSX2 `_vuFlushAll` waits for outstanding IALU reservations at E. The test therefore expects one additional cycle for the final pending destination to retire; the EE cycle counter receives the same consumed-cycle increment.
- Added tests for all seven operations, VI0 and upper-half preservation, M slice boundaries, FINISH_TO_E resumption after M, E delay/flush, and rejection of decoded but unsupported LQ/upper-vector pairs.
- Actions initially caught a mistaken positive operand in the unsupported-lower test and then a one-cycle-too-low E-flush expectation. Both were corrected from the observed semantics; no execution code was weakened to make the tests pass.
- GitHub Actions [run 43189469757](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43189469757): clean configure/build and **14/14 CTest passed**.
- This is a deliberately narrow micro-engine foundation, not full VU0 support. Branches, data memory, I/D/T modes, upper vector ops, full dual-issue ordering, VF/flag forwarding and FMAC/FDIV/EFU pipelines remain unsupported.
- Next: implement lower integer branches with explicit branch-delay state and IALU read interlocks; keep vector/memory paths unsupported until separately validated.


## 2026-11-12 — Conservative static JALR target recovery

- Added `collect_static_jalr_targets`, a forward constant-propagation pass over each caller's basic-block CFG. It recognizes LUI/ORI/ANDI/XORI/ADDIU/DADDIU and selected SPECIAL integer arithmetic/shift operations. GPR0 is hardwired to zero; unmodelled writes and memory loads invalidate affected constants.
- A JALR target is collected only when its source register is known at the instruction, nonzero and four-byte aligned. The target is sampled before the JALR delay slot. Branch-likely CFG edges carry separate taken and annulled-fallthrough states, so a delay-slot write is not applied to a not-taken path.
- Integrated the pass into `ee_recomp`'s discovery worklist as a fixed-point process. Static JALR call targets are validated against executable ELF ranges, added as entries, decoded, and scanned again for further calls. Dynamic targets and register values derived from unmodelled memory/table loads remain unresolved.
- Function discovery now pre-collects leaders from all decoded conditional branches, branch-likely instructions and direct jumps. Linear block construction stops at those leaders even before they have been visited; a later-discovered target inside an already scanned block is split into its own block with a connecting predecessor edge. This fixes overlapping basic blocks that hid merge predecessors from the data-flow analysis.
- Added `function_discovery_test` with fixtures for LUI/ORI, signed LUI/ADDIU, $zero, JALR target sampling before its delay slot, unknown load clobbers, conflicting/equal CFG predecessor constants, and branch-likely delay-slot annulment.
- CI initially exposed C++ scoped-enum conversions in the new analysis, a missing parenthesis in REGIMM link handling, a fixture namespace issue, and then the overlapping-basic-block bug. Those were corrected from the compiler/test output. The new branch-likely fixture is included in the final passing run.
- GitHub Actions [run 43207855948](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43207855948): clean configure/build and **15/15 CTest passed**, including `function_discovery`.
- The preserved USA `SLUS_214.42` full-image validation (7,849 functions) predates this JALR recovery. Do not update or reuse that count as a result of the new pass until the full ELF is re-run. A fresh full-image native-link validation is the next check.
- Remaining limits: JALR targets that depend on GOT/table loads, runtime values, interprocedural register states, or unsupported transformations are not guessed. Function discovery is still conservative and is not proof that every stripped-image function is found.


## 2026-11-12 — VU0 integer branches, JR/JALR and VI backup timing

- Cross-checked lower branch control flow against PCSX2 `VU0microInterp.cpp` and `VUops.cpp`: TPC advances before branch target calculation; B/BAL use signed 11-bit encoded offsets with bit 10 as the sign bit; branch issue uses a two-pair delay pipeline; branch-in-delay-slot targets are deferred until after the first target instruction.
- Extended the executable upper-NOP/lower-integer slice to B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ and IBGEZ. JR/JALR sample the low 16-bit VI target in instruction-slot units before any JALR link write; JALR supports the Is == It overlap.
- Added the PCSX2 VI-backup window to the sidecar. IALU writes preserve the pre-chain low halfword for two cycles; repeated writes to the same VI retain the original value and refresh the window; a write to another VI replaces the backup register. Conditional branch reads use the backup value while active. E termination clears the window.
- Appended the backup counter/register/value to the VU0 sidecar. C++ layout assertions and the Python ctypes mirror now agree on offsets 8524/8528/8532 and the 8544-byte, 16-byte-aligned state size; Python includes explicit tail padding because ctypes natural alignment is 8 bytes.
- Added direct runtime cases for B/BAL delay slots, JR, JALR including Is/It overlap, JALR in a branch delay pair, every conditional integer branch, negative relative offsets/wraparound, and both branch-read VI-backup cases (active backup and replacement/chain behaviour).
- GitHub Actions [run 43210588688](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43210588688): clean build and **15/15 CTest passed**.
- This remains a restricted lower-integer/branch engine, not a full VU0 interpreter. VU data-memory operations, I/D/T flags, upper vector operations, general dual-issue dependencies, VF forwarding and FMAC/FDIV/EFU pipelines remain unsupported. The new tests are direct runtime regressions, not yet a full differential execution harness against PCSX2.
