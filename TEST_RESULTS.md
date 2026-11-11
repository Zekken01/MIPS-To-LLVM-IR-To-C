# Test Results

## 2026-11-11 clean recovery build

Environment: GCC 14.2.0, CMake/Ninja, Python 3, llvmlite/LLVM 20.1.8.

Commands:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/ee_decoder_test
```

Results on the reconstructed source snapshot:

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **4/4 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`).
- Decoder regressions: **269 passed, 0 failed**.
- The generated LLVM runtime fixture parsed, verified, and JIT-executed at module-builder optimization levels O0–O3.

The focused LLVM runtime fixture exercises HI1/LO1 transfers, supported MMI logical operations, same-block aliasing, upper-GPR preservation, and cross-block register writeback.

These results validate this recovered source snapshot. They do **not** establish that the inaccessible 2/4 worktree was repaired.

## Historical real-game validation

Preserved notes report 6,797 discovered functions from USA `SLUS_214.42`. Full-game LLVM parse/verify and O0 host-object emission were completed before the final narrow CFG liveness refinement. After that refinement, the real-game entry function at `0x00100008` was verified and emitted as an 800-byte host object. A complete 6,797-module re-verification after that final refinement was not recorded.

The in-project object emitter remains a stub; llvmlite JIT/object checks are validation harnesses, not a runnable static recompiler.
