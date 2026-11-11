# Resume Point

## Current baseline

The branch is a reconstructed baseline from the preserved 2026-11-11 09:57 MMI/HI1/LO1 follow-up, with subsequent source and test work committed on this recovery branch. Do not merge it into `main` until its provenance and current source have been reviewed.

Latest verified CI: [run #147](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43109193936) — clean configure, build, and **9/9 CTest passed**.

The verified test set covers:
- Decoder and native multi-function regressions.
- MMI0/MMI1, HI1/LO1, MULT1/MULTU1, DIV1/DIVU1 LLVM JIT execution.
- JAL/JALR/JR delay-slot and link timing, branch condition sampling, branch-likely annulment, and branch-and-link cases.
- Separate caller/callee return and continuation, one-level recursive JAL, cross-function JALR, unknown-PC handling.
- Host dispatcher PC ownership, caller continuation after return, external return, unresolved PC, invalid table shape, and dispatch-budget exhaustion.

## Remaining priorities

1. **Full-game revalidation:** repeat LLVM parse/verify over all 6,797 functions from USA `SLUS_214.42` after the latest CFG, liveness, and dispatcher changes. The preserved 6,797-module result predates these later control-flow changes.
2. **Native backend integration:** implement a real LLVM object emitter and produce a stable array of native function entry points corresponding to `s_recompiled_elf::functions`. The new `ee_dispatch_run()` API accepts that metadata plus the parallel entry array, but the in-project `object_emitter::emit_object()` remains a stub.
3. **Semantic differential coverage:** compare more control-flow and instruction edge cases against PCSX2/reference EE execution, not only structural LLVM verification and synthetic JIT fixtures.
4. **Runtime completeness:** continue the EE memory/TLB/MMIO model, exception integration, and VU0/COP1 implementation. Unknown or inexact instructions must remain explicitly unsupported.

The exact later temporary worktree associated with the historical 2/4 CTest report was never exported. This branch is a reconstructed source baseline, not recovery of that inaccessible working directory.

## Reproduction

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/ee_decoder_test
```
