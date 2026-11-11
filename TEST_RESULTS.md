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


## 2026-11-11 MMI / MAC follow-up

Environment: GitHub Actions `ubuntu-latest`, GCC/CMake/Ninja, isolated Python virtual environment with `llvmlite`.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **4/4 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`).
- LLVM JIT runtime test: PASS at O0, O1, O2, and O3.
- Runtime assertions cover:
  - HI1/LO1 transfer ordering and preservation of upper GPR halves.
  - Signed MADD and unsigned MADDU accumulation into HI:LO.
  - Signed MADD1 and unsigned MADDU1 accumulation into HI1:LO1.
  - MFHI/MFLO and MFHI1/MFLO1 readback.
  - PADD/PSUB/PMAX word, halfword, and byte lane behavior.
  - PAND/PXOR/POR/PNOR, source/destination aliasing, and cross-CFG-block writeback.

The MADD/MADDU behavior is implemented from the EE Core Instruction Set Manual v6.0 operation definitions: the low 32-bit halves of the appropriate HI/LO pair form the 64-bit accumulator; the low and high result words are sign-extended into LO/HI, and the low result word is also written to rd.


## 2026-11-11 Pipeline-1 multiply follow-up

- Clean configure/build: PASS.
- CTest: **4/4 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`).
- LLVM JIT runtime: PASS at O0–O3.
- Added runtime coverage for signed `MULT1` and unsigned `MULTU1`, including HI1/LO1 writeback, sign-extension, and MFHI1/MFLO1 readback.

## 2026-11-11 Pipeline-1 division follow-up

Environment: GitHub Actions `ubuntu-latest`, GCC/CMake/Ninja, isolated Python virtual environment with `llvmlite`.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **5/5 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`, `div1_ir_runtime`).
- Existing MMI runtime fixture: PASS at O0–O3.
- New DIV1/DIVU1 LLVM JIT fixture: PASS at O0–O3 for all five cases:
  - Signed quotient/remainder (`-17 / 5`).
  - Signed overflow (`INT_MIN / -1`).
  - Signed divide-by-zero preservation.
  - Unsigned quotient/remainder (`0xfffffffe / 3`).
  - Unsigned divide-by-zero preservation.
- Each case verifies HI1/LO1 state and MFHI1/MFLO1 readback.
- GitHub Actions run: [43090955314](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43090955314).

## 2026-11-11 MMI1 packed-operation follow-up

Environment: GitHub Actions `ubuntu-latest`, GCC/CMake/Ninja, isolated Python virtual environment with `llvmlite`.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **6/6 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`, `div1_ir_runtime`, `mmi1_ir_runtime`).
- New MMI1 LLVM JIT runtime test: PASS at O0–O3.
- Runtime coverage:
  - PABSW/PABSH with signed-minimum saturation.
  - PCEQW/PCEQH/PCEQB all-ones equality masks.
  - Signed PMINW/PMINH.
  - PADDUW/PADDUH/PADDUB unsigned saturation.
  - PSUBUW/PSUBUH/PSUBUB unsigned floor-at-zero saturation.
  - In-place source/destination aliasing.
- The game entry-point encoding `0x70000c28` now decodes as supported PADDUW.
- Remaining MMI1 operations stay unsupported: PADSBH, PEXTUW, PEXTUH, PEXTUB, and QFSRV.
- GitHub Actions run: [43093331006](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43093331006).

## 2026-11-11 MMI1 completion — validation pending

- Implemented the remaining defined MMI1 operations: PADSBH, PEXTUW, PEXTUH, PEXTUB, and QFSRV.
- Added MTSAB/MTSAH decoding, lifting, and SA state updates used by QFSRV.
- Extended the MMI1 JIT fixture to cover all 18 non-reserved MMI1 sub-ops and multiple byte/halfword SA settings at O0–O3.
- **Validation status:** GitHub Actions runs for this latest source revision are queued and have not yet produced a result. The prior 6/6 green result applies to the earlier 13-operation MMI1 subset only.
