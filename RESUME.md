# Resume Point

## Current baseline

This branch is a reconstructed baseline from the preserved 2026-11-11 09:57 MMI/HI1/LO1 follow-up, with subsequent decoder, lifting, control-flow, dispatcher, and validation work. Do not merge it into `main` until source provenance and the latest changes have been reviewed.

Latest fully verified CI: [run #191](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43117522952), **12/12 CTest passed**, including native object emission, linking, and dispatcher execution.

Latest USA `SLUS_214.42` validation:
- **7,849 functions discovered and emitted**.
- **417,738 unique instruction PCs** in the function-scoped CFG set.
- **416,648 supported / 1,090 unsupported** instructions (**99.7% decoder/lifter classification**).
- **7,849/7,849 LLVM modules parsed and verified** with LLVM 20.1.8.
- **7,849/7,849** host objects emitted at `opt=0`, totalling **35,026,424 bytes**.
- All 7,849 objects' defined symbols matched the generated function metadata.
- All 7,849 per-function objects linked with runtime and generated dispatcher glue into a **28,618,808-byte x86-64 shared library**.
- The linked library exports `ee_game_run` and `ee_fn_00100008`, loads with `ctypes.CDLL`, and passes `ldd -r` with no unresolved dynamic symbols.

The full-image native-link report is in `FULL_GAME_NATIVE_LINK_VALIDATION.md`. This is a linked translated-code library, not a complete runnable PS2 title: the full title was not executed, and BIOS/IOP, GS, DMA, TLB/MMIO, interrupts, and other console services remain incomplete. The in-tree `object_emitter::emit_object()` is still a stub; llvmlite generated the persisted per-function objects.

Control-flow regressions cover JAL/JALR/JR target and link ordering, delay-slot effects, ordinary and likely branch semantics, branch-and-link, caller/callee continuations, recursive JAL, dynamic cross-function JALR, PC-zero entry, exception resume PCs, and host dispatcher budget/unknown-target handling. COP1 comparison JIT coverage exercises C.F/C.EQ/C.LT/C.LE, EE operand normalisation, NaNs, signed zero, and FCC0-only FCSR updates. Scalar compare JIT coverage checks signedness, sign-extended immediates, source/destination aliasing, `$zero`, and upper GPR halves.

## Remaining priorities

1. **COP1 completion:** exact rounding/conversion modes and FCSR exception/sticky flag semantics. Keep uncertain forms unsupported.
2. **VU0/COP2:** implement macro/transfer semantics only with exact pipeline, flag, and 128-bit state behaviour.
3. **Runtime completeness:** EE memory/TLB/MMIO, scratchpad/ROM aliases, alignment exceptions, DMA, interrupts, plus BIOS/IOP/GS host services.
4. **Semantic validation:** differential instruction and function testing against PCSX2's interpreter. LLVM verification and native linking do not establish R5900 equivalence.
5. **Production backend:** replace the current external llvmlite object-emission path with an integrated LLVM backend or supported `llc`/LLVM library path; keep the current script as a validation/reference path until then.

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
