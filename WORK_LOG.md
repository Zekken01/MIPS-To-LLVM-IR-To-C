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
