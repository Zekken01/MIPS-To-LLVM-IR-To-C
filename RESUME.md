# Resume Point

## Current baseline

This branch is a reconstructed baseline from the preserved 2026-11-11 09:57 MMI/HI1/LO1 follow-up, with later decoder, lifting, control-flow, dispatcher, and validation work. Do not merge it into `main` until source provenance and the latest changes have been reviewed.

Latest CI baseline: [run #155](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43110647060), **9/9 CTest passed**.

Latest USA `SLUS_214.42` validation:
- **7,849 functions discovered and emitted**.
- **417,738 unique instruction PCs** in the function-scoped CFG set.
- **409,530 supported / 8,208 unsupported** instructions (98.0%).
- **7,849/7,849 LLVM modules parsed and verified** with LLVM 20.1.8.
- **7,849/7,849** modules also passed llvmlite native object generation at `opt=0`, with 34,220,040 aggregate object bytes.

Control-flow regressions cover JAL/JALR/JR target and link ordering, delay-slot effects, ordinary and likely branch semantics, branch-and-link, caller/callee continuations, recursive JAL, dynamic cross-function JALR, PC-zero entry, exception resume PCs, and host dispatcher budget/unknown-target handling.

## Remaining priorities

1. **Real native backend:** implement the project `object_emitter::emit_object()` and save actual per-function host objects. The llvmlite check emitted object bytes in memory only; no per-function object set was linked.
2. **Runnable dispatcher integration:** connect `ee_dispatch_run()` to generated native function entry points and the real runtime. The dispatcher API and synthetic native tests exist, but the generated functions are not yet loaded/linked into it.
3. **Address the 8,208 unsupported instructions:** rank the remaining opcode/sub-op coverage, then add exact semantics and reference/JIT tests. Keep uncertain encodings unsupported.
4. **Runtime completeness:** continue EE memory/TLB/MMIO and exception integration, then VU0/COP1 behavior.
5. **Broader validation:** after each decoder/lifter/backend change, repeat full-image IR verification and object emission on `SLUS_214.42`.

The exact later temporary worktree associated with the historical 2/4 CTest report was never exported. This branch remains a reconstructed source baseline, not recovery of that inaccessible working directory.

## Reproduction

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/ee_decoder_test
```

Full-image validation (after building `ee_recomp` and installing `llvmlite`):

```sh
./build/ee_recomp SLUS_214.42 build/recompiled
python3 tools/verify_ir.py 'build/recompiled/*.ll' --progress=250
python3 tools/verify_ir.py 'build/recompiled/*.ll' --emit-opt=0 --progress=250
```
