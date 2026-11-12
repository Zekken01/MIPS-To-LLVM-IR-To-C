# Resume Point

## Current baseline

This branch is a reconstructed baseline from the preserved 2026-11-11 09:57 MMI/HI1/LO1 follow-up, with subsequent decoder, lifting, control-flow, dispatcher, and validation work. Do not merge it into `main` until source provenance and the latest changes have been reviewed.

Latest fully verified CI: [Actions run #43207855948](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43207855948), **15/15 CTest passed**, including the new function-discovery suite, CFG/data-flow regressions, VU0 runtime, COP1/COP2 JIT tests, native object emission/linking, dispatcher execution, and the MMI/HI1/LO1 regression suites.

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

## Function discovery and indirect calls

- Entry discovery seeds the ELF entry point and defined `STT_FUNC` symbols in executable segments, then harvests direct `JAL` and REGIMM branch-and-link targets from reachable decoded code.
- Discovery now iterates to a fixed point: every newly decoded callee is scanned for further direct calls and statically resolvable `JALR` calls.
- `collect_static_jalr_targets` runs forward constant propagation over each caller CFG. It recognizes common address-building sequences (`LUI`, `ORI`, `ANDI`, `XORI`, `ADDIU`, `DADDIU`, and selected SPECIAL arithmetic/shift operations). A target is accepted only when the target register remains a known, aligned, nonzero constant at the `JALR` itself.
- CFG joins discard a register constant when reachable predecessors disagree. JALR samples its target before the delay-slot instruction executes. Branch-likely propagation uses separate taken/fallthrough states so an annulled delay slot cannot contaminate the not-taken path.
- CFG construction pre-collects decoded branch/jump leaders and splits blocks at targets even when a target is found inside a block already scanned. This avoids overlapping blocks and preserves all predecessors for data-flow.
- `function_discovery_test` covers constant address construction, `$zero`, delay-slot sampling, register clobbers, equal/different predecessor values, and branch-likely annulment. These are static-analysis unit tests; a fresh full-USA-ELF run has not yet measured how many additional function entries JALR recovery finds.
- Dynamic function pointers loaded from memory, GOT/table lookups, runtime-computed targets, and unsupported register-value transformations remain unresolved. Do not interpret static discovery as proof that every function in a stripped ELF has been found.

The 7,849-function full-image figures above are the preserved pre-JALR-recovery validation baseline. Re-run the complete USA ELF analysis and native-link validation before updating those figures or claiming a new total.

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
- Fetch also classifies lower and upper operations using the PCSX2 primary/secondary tables. Lower primary `0x3c` dispatches through the lower sub-op/T3 tables; upper primary `0x3c..0x3f` dispatches through the four FD tables. Unassigned entries return `UNKNOWN`; this is classification, not execution.
- Runtime transfer helpers apply synchronization before reads/writes; cover VF0, GPR0, CFC2 R and CTC2 special-register cases; and fail closed when the required scheduler/control callback is missing or fails.
- BC2 reads VPU_STAT bit 8 and has ordinary/likely branch JIT coverage, including delay-slot execution and annulment.
- `ee_vu0_scheduler_sync_callback` provides bounded catch-up, finish-to-E, and wait-to-E-or-M orchestration, including VU1 catch-up and EE event callbacks. Interlocked waits add only the VU0 cycles consumed to the original EE cycle counter; they do not collapse EE time to the VU0 counter unless taking PCSX2's separate early M-bit/already-ahead path.
- `ee_vu0_execute_ialu_callback` executes lower IADD/ISUB/IAND/IOR/IADDI/IADDIU/ISUBIU and lower B/BAL/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ when paired with upper NOP. The IALU path performs 16-bit VI writes, keeps VI0 hardwired, reserves destinations in the four-entry queue, retires ready reservations, processes E's two-pair delay and flushes pending writes at termination. The branch path includes relative-target calculation, branch-delay state, nested-branch deferral and IALU source read stalls.
- Each engine execution slice returns at an M-flagged pair. The outer scheduler decides whether M ends the wait: WAIT_TO_E_OR_M returns, while FINISH_TO_E invokes another slice and continues toward E. The integration tests cover resuming after M, E delay execution and the final IALU flush cycle.
- This is **not** a general VU0 interpreter. The lower branch implementation does not yet have a dedicated runtime regression suite, so treat that path as provisional despite the overall CI pass. Lower JR/JALR, VU data-memory operations, I/D/T flags, upper vector operations, dual-issue dependencies, VF forwarding and FMAC/FDIV/EFU pipelines remain unsupported. Unsupported pairs fail closed without consuming their micro-PC.
- VU0 SPECIAL1/SPECIAL2 macro instructions remain UNSUPPORTED. Do not enable them until dual-issue scheduling and the FMAC/FDIV/EFU pipelines, flags, ACC/Q/P forwarding and branch timing are implemented.

The VU0 scheduler/micro-op foundation, COP2 JIT, and function-discovery/data-flow regressions passed **15/15** in [Actions run #43207855948](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43207855948).

## Remaining priorities

1. **Full-image discovery revalidation:** rerun `SLUS_214.42` through the iterative JALR discovery path, compare the new entry list against the preserved 7,849 baseline, and repeat LLVM verification/object linking.
2. **JALR coverage expansion:** add conservative patterns for additional proven address construction while preserving the all-predecessors-agree rule; keep GOT loads and runtime-dependent pointers unresolved until their memory/relocation semantics are modelled.
3. **VU0 branch validation:** add direct runtime tests for B/BAL and every conditional integer branch, delay-slot execution, nested branches, and IALU read interlocks. Do not promote the lower branch slice to a validated status until those tests and PCSX2 comparison pass.
4. **VU0 memory and vector issue:** add LQ/SQ/ILW/ISW and a carefully tested dual-issue model before enabling vector arithmetic or macro instructions.
5. **Runtime completeness:** EE memory/TLB/MMIO, scratchpad/ROM aliases, alignment exceptions, DMA, interrupts, plus BIOS/IOP/GS host services.
6. **Semantic validation:** differential instruction and function testing against PCSX2's interpreter. LLVM verification and native linking do not establish R5900 equivalence.
7. **Production backend:** replace the current external llvmlite object-emission path with an integrated LLVM backend or supported `llc`/LLVM library path; keep the current script as a validation/reference path until then.

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
