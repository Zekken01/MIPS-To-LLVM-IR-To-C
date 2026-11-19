# M2.7 — Strict bounded native VU1 and GIF PATH1, first batch

**Status:** partial implementation; real USA SDBZ game boot unverified.
**Branch:** feature/sdbz-hle-first-bootstrap; draft PR #142 (unmerged).
**Reference:** twist84/ps2-web-recomp:SDBZ runtime/src/vu_micro.cpp and runtime/include/ps2/vu_ops.hpp (Samir Nuri, MIT 2026); retained license attribution.

## Implemented

- 16 KiB resident VU1 code/data, microinstruction pairs, bounded E-bit delay-slot execution, staged upper VF writeback, I-bit, partial floating-point vector arithmetic, vector memory operations, VI integer operations, VI branch-delay view, TOP/ITOP, integer branches, TPC and XGKICK.
- XGKICK walks real GIFtags and payload in VU1 data (up to 1024 qwords, wraps data memory) and forwards only an EOP-complete packet to a separate GIF PATH1 parser sharing the original GS/WebGPU event sink.
- MSCAL launches the real MPG-uploaded VU1 microinstructions; MSCNT resumes at real TPC after a prior accepted program. FBRST retains resident code/data and VU1 registers.
- VIF1 stages VU1 state/data before GIF emission; rejected packets leave VIF1 DMA active, with micro-PC and upper/lower words logged.
- Mixed PATH1/PATH2 within one DMA transfer is explicitly rejected pending a transactional per-path arbiter.

## Deliberately unsupported

This is **not full VU1 parity**. Unimplemented upper arithmetic and ACC operations (MADD/MSUB, CLIP, etc.), MAC/status flags, Q/P latency, most lower specials (EFU, flags, RNG, division, etc.), exact instruction stalls/hazards, MSCALF synchronization, GIF PATH1/2/3 arbitration, and VIF IRQ still fail closed. There is no claim of complete PS2 VU correctness or original game progression. Complete the upstream interpreter in coherent tested batches before relaxing these gates.

The original 13:53 boot showed VIF1 source-chain CHCR=0x1c5 and TADR=0x00771400, but was captured before source-prefix diagnostics existed. No game VIF opcode is proved reached. Only a new timestamp-matched Windows boot batch can prove progression.

## Regression

The ee_vif1_device test adds a real 3-pair VU1 MPG program (IADDIU, XGKICK with E-bit and delay slot) and actual in-memory GIFtag/body, proves PATH1 events, and rejects an unsupported upper opcode without emitting GIF. A standalone direct VU1 arithmetic/E-bit test checks VF results and two executed pairs. Existing TTE, UNPACK, PATH2 and MMIO tests remain. CI completion must be verified on the final exact head; tests are not evidence of game boot.

## Next batch

Port upstream VU1 ACC/arithmetic and flag/Q/P pipelines, with precise hazards, branch and register validation; then unify GIF PATH1/2/3 arbitration and VIF/DMAC stalls rather than skipping them.
