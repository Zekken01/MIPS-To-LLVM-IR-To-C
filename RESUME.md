# Resume Point

## Current baseline

This branch is the reconstructed 2026-11-11 09:57 MMI/HI1/LO1 follow-up source snapshot. A clean GCC/Ninja build passes all four registered CTest suites; decoder coverage is 269/269.

## Next priorities

1. Preserve the exact 2/4 failure details as unresolved provenance; the later live worktree was never exported.
2. Audit recursive JAL, delay-slot discovery, JALR/JR targets, return-address tracking, and cross-function dispatcher state.
3. Expand MMI only with exact reference semantics and compiled-LLVM runtime tests; leave uncertain forms unsupported.
4. Repeat full LLVM verification over all 6,797 game functions after CFG/liveness changes.
5. Integrate a real LLVM object backend and dispatcher. The in-project `object_emitter::emit_object()` is still a stub.

## Reproduction

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/ee_decoder_test
```
