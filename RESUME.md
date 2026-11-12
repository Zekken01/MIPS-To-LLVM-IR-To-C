# Resume Point

## Current baseline

This branch is a reconstructed baseline from the preserved 2026-11-11 09:57 MMI/HI1/LO1 follow-up, with subsequent decoder, lifting, control-flow, dispatcher, and validation work. Do not merge it into `main` until source provenance and the latest changes have been reviewed.

Latest fully verified CI: [Actions run #43183141780](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43183141780), **14/14 CTest passed**, including native object emission/linking, dispatcher execution, COP1 JIT tests, VU0 runtime scheduler tests, COP2 transfer/BC2 JIT tests, and the MMI/HI1/LO1 regression suites.

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

Control-flow regressions cover JAL/JALR/JR target and link ordering, delay-slot effects, ordinary and likely branch semantics, branch-and-link, caller/callee continuations, recursive JAL, dynamic cross-function JALR, PC-zero entry, exception resume PCs, and host dispatcher budget/unknown-target handling.

COP1 JIT coverage now includes the supported R5900 instruction-table forms:
- MFC1/MTC1 and CFC1/CTC1, including cross-block GPR liveness for CTC1.
- C.F.S/C.EQ.S/C.LT.S/C.LE.S, signed zero, denormal and NaN normalisation, and FCC0/FCSR preservation.
- ADD.S/SUB.S/MUL.S/DIV.S, SQRT.S/RSQRT.S, MAX.S/MIN.S, ABS.S/NEG.S.
- ADDA.S/SUBA.S/MULA.S, MADD.S/MSUB.S, MADDA.S/MSUBA.S, and CVT.W.S/CVT.S.W.
- FCSR cause/sticky semantics, denormal handling, signed infinities/NaNs, saturation and accumulator-path distinctions.
- BC1 condition sampling before an ordinary delay slot and BC1FL delay-slot annulment.

## Latest COP2/VU0 status

See [`COP2_RESEARCH.md`](COP2_RESEARCH.md) for the PCSX2 reference and semantic caveats.

- QMFC2/QMTC2/CFC2/CTC2 and BC2F/T/FL/TL are now decoder-VALID for their documented encodings and lowered through the generated LLVM runtime-helper calls.
- The aligned `s_ee_vu0_state` sidecar carries VI[32], ACC, Q/P, visible control/flag values, TPC/micro-PC, cycle counters and E/M/running state, plus separate 4 KB VU data and micro-program memories. The established `s_ee_cpu_state::m_vu0[32]` remains the VF bank, so the previous CPU/LLVM ABI offsets are preserved.
- `ee_vu0_fetch_micro_instruction` fetches paired lower/upper words, decodes I/E/M/D/T flags, advances the byte PC by eight and wraps at 4 KB. C++ and Python ABI layout checks cover the appended memories.
- Runtime transfer helpers apply synchronization before reads/writes; cover VF0, GPR0, CFC2 R and CTC2 special-register cases; and fail closed when the required scheduler/control callback is missing or fails.
- BC2 reads VPU_STAT bit 8 and has ordinary/likely branch JIT coverage, including delay-slot execution and annulment.
- `ee_vu0_scheduler_sync_callback` now provides bounded catch-up, finish-to-E, and wait-to-E-or-M orchestration, including VU1 catch-up and EE event callbacks. Interlocked waits add only the VU0 cycles consumed to the original EE cycle counter; they do not collapse EE time to the VU0 counter unless taking PCSX2's separate early M-bit/already-ahead path. It does **not** execute VU micro-instructions. A real host VU0 engine remains to be supplied.
- VU0 SPECIAL1/SPECIAL2 macro instructions remain UNSUPPORTED. Do not enable them until the engine models dual-issue scheduling and FMAC/FDIV/EFU pipelines, flags, ACC/Q/P forwarding and branch timing.

The scheduler, micro-memory/fetch and existing regression suites passed **14/14** in [Actions run #43183141780](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43183141780).

## Remaining priorities

1. **VU0 micro-op decoder/interpreter:** decode the paired lower/upper words and implement instruction-step orchestration, delayed VI/VF writes, branch delay state, E/M termination and pipeline hazard tracking. Then implement a conservative first execution subset and differential-test it against PCSX2.
2. **VU0 macro instructions:** after the engine exists, add macro families incrementally with dual-issue/pipeline/flag tests. Keep undocumented or unmodelled forms unsupported.
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
