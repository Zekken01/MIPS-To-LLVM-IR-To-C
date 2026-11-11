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

