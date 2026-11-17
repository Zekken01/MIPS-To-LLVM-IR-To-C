# Super Dragon Ball Z (US) — Native Boot Development Plan

**Repository:** [twist84/MIPS-To-LLVM-IR-To-C](https://github.com/twist84/MIPS-To-LLVM-IR-To-C)  
**Status date:** 2026-11-17  
**Baseline:** `main` at `d81a8e2ae5437434d485883ca2afd720e0e46152` (squash-merged [PR #127](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/127))  
**Goal:** Boot and eventually play **Super Dragon Ball Z (USA, `SLUS_214.42`)** using recompiled EE code, genuine guest IOP execution, and a native runtime, without PCSX2.

> This is a milestone and acceptance plan, **not** a statement that the game currently boots or renders a usable frame. Update the checkpoint and evidence as new PRs merge. Re-check `main` and any newer work before acting on these instructions.

## 1. Architecture and non-negotiable invariants

The architecture should remain closer to [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) than PCSX2:

- **EE/R5900:** statically recompiled via MIPS → LLVM IR → host-native objects; native dispatcher owns each reachable guest entry.
- **IOP/R3000:** execute genuine guest code with the lightweight interpreter and [OpenPS2BIOS](https://github.com/TamaribaEmu/OpenPS2BIOS) where supported.
- **CDVD/SIF/DMA:** use the original disc sectors and actual guest-visible registers, data, callbacks, and interrupts.
- **GIF/GS:** consume original guest GIF packets and maintain real GS state/VRAM; native primitive rendering can follow once demanded by guest execution.
- **PCSX2:** hardware behavior reference and differential oracle, **not** the runtime architecture.

Never fake `CHCR.STR` completion, an IRQ, a semaphore signal, ROM0 readiness, sector contents, GS acceptance, or a frame merely to increase EE dispatches. A dispatch-limit stop, untranslated entry, true hardware stall, and successful boot are different outcomes.

Keep source under Git; **never commit the game ISO/ELF, ROM0 BIOS, copyrighted assets, or bulky generated native objects**.

## 2. Confirmed baseline and evidence

The following were implemented and merged by 2026-11-17:

| PR | Validated work |
|---|---|
| [#121–#124](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/124) | CDVD reset MMIO, N-command parameter FIFO, real DMA3 request state |
| [#125](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/125) | Actual raw-ISO CDVD reads into IOP RAM via DMA3 |
| [#126](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/126) | Genuine ROM0 SIF/WaitSema response; SIF1 DMA10 terminal-tag semantics; R3000/EE startup support; guest-anchored function-pointer discovery; 256-function cached dispatcher chunks; GIF normal DMA → GS |
| [#127](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/127) | GIF channel 2 source-chain **END** tag transfer, real inline QWC delivery, bounds/consumer rejection tests |

**Genuine IOP → EE handshake evidence:** EE DMAC channel 5 IRQ enters guest handler `0x00178068`; `iSignalSema(8)` changes count 0 → 1; blocked `WaitSema(8)` resumes and consumes that token. The SIF readiness blocker is closed.

**Genuine disc reads:** LBA 16 (ISO PVD), LBA 261 (1 sector), LBA 289 (135 sectors), LBA 450 (4 sectors beginning `7f 45 4c 46`, ELF). The source is a real 2048-byte-sector US ISO.

**GIF/GS evidence:** normal `CHCR=0x101` transfers reached the PATH3 parser; 32 GIF tags, 16,384 IMAGE QWC and 65,536 written GS VRAM pixels were recorded. The next observed source-chain transfer used `CHCR=0x105`, `TADR=0x00503310`, a 9-QWC `END` tag (`0x70000009`), and was accepted by the GS consumer.

**Last confirmed focused full-game replay:** **12,168 EE dispatches**, final guest PC **`0x00102E24`**, reason **untranslated native entry** (`DISPATCH`, status 0). This occurred *after* the source-chain DMA blocker at `0x00105034` was passed; it was **not** evidence of complete gameplay. A later `END` tag (19 QWC at `0x005033B0`) was observed in the diagnostic stream; do not infer another missing DMA mode from this without a fresh guest trace.

Historical local full-library work reached approximately **8,047 LLVM-verified translated modules**. These were scratch build artifacts, not source checked into `main`. The source-controlled pipeline is authoritative.

## 3. Stage 0 — Establish a reproducible, provenance-safe replay

**Objective:** reproduce the merged baseline without overwriting existing work.

1. Fetch `main`; inspect open PRs, changes since `d81a8e2`, and the working tree before creating a feature branch.
2. Locate the original `SLUS_214.42`, US ISO, and genuine OpenPS2BIOS 0.6 ROM0. Check their provenance; do not substitute another region or infer region from a reused ELF filename.
3. Verify the ELF against the known US SHA-256: `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`. Previously verified OpenPS2BIOS ROM0 SHA-256: `3702baa508ff40498fab4a95429a2ae1f903d11f5bbcdd4e1a87dbe9f5d21a84`.
4. Check build prerequisites (CMake, modern C++, Python, LLVM/llvmlite where used). Read current `--help` rather than assuming an old invocation remains valid.
5. Reuse the existing guest IR/object cache **only when its provenance and runtime fingerprints validate**. After runtime changes, relink/refresh correctly; never disable fingerprint checks to force an old shared library to load.
6. Preserve the ELF hash, ROM hash, ISO file/hash or verified identity, runner flags, commit SHA, stop reason, and at least the last 64 dispatches in structured JSON and text logs.

The runner reads **`SDBZ_IOP_ROM0` from the environment**; `tools/run_game.py` does not currently expose `--iop-rom0`. The direct `ee_game_runner` does accept `--iop-rom0=FILE`.

Example, with paths adapted to the current environment:

```bash
export SDBZ_IOP_ROM0=/path/to/verified/openps2bios-v0.6-rom0.bin
python tools/run_game.py /path/to/SLUS_214.42 \
    --build-dir=build \
    --work-dir=build/sdbz-us-native-boot \
    --functions=all \
    --hle-startup --kseg-ram \
    --disc-image=/path/to/Super_Dragon_Ball_Z_USA.iso \
    --emit-jobs=8 --emit-opt=0 \
    --max-dispatches=100000 --loop-budget=1000000 \
    --sif-poll-limit=2048 --timeout-seconds=120 \
    --auto-expand=128 --auto-expand-table=24 \
    --report=build/sdbz-us-native-boot/first-boot.json
```

If no authentic raw ISO is available, explicitly mark real CDVD/boot acceptance **blocked**. A loose folder or synthetic fixture can test infrastructure but cannot prove the original ISO sector response.

**Acceptance:** the runner demonstrably uses genuine ROM0, original US sector data, and the latest native code. Identify and reproduce either the known `0x00102E24` stop or a properly explained newer stop; do not attribute a different run's count to this build.

## 4. Stage 1 — Close the next native entry gap: `0x00102E24`

**Objective:** pass the observed untranslated PC via actual native translation, rather than changing guest state.

1. Read `tools/run_game.py` and its `append_function()`, `covered_pcs()`, `pointer_table_siblings()` and runtime-stop parsing. Inspect `tools/link_native_objects.py`'s 256-function chunking and runtime cache validation.
2. Check if `0x00102E24` is aligned, inside the game's executable ELF ranges, already owned by a CFG block, or actually a guest callback/indirect jump into the middle of a discovered function. Compare the original ELF bytes and incoming control flow.
3. Let `--auto-expand` add the genuine guest-reached entry; if it is in a credible contiguous pointer table, batch only validated siblings under `--auto-expand-table`. Avoid predeclaring arbitrary executable-looking words as functions.
4. When shared CFG blocks overlap, keep the original owner and publish only new guest entry PCs; preserve the full separately lifted module's correct control flow.
5. Run `verify_ir.py` incrementally, relink with verified chunk caches, and replay. If discovery remains systematically incomplete, improve the discovery algorithm and add a small test fixture proving the omission is fixed.
6. Capture the **first new genuine blocker** and compare against the last boot trace. Do not call a higher count progress if it only consists of a poll loop.

**Acceptance:** a correctly translated/owned `0x00102E24` executes, guest control flow continues, targeted IR/dispatcher tests pass, and the real SDBZ replay advances to a new explained stop (or reaches a genuine runnable state). Keep its exact inputs, PC, stop reason and evidence.

## 5. Stage 2 — Classify the next guest dependency, then implement only what is required

Use this triage order after each replay:

1. **Untranslated EE PC:** fix coverage or ownership in the native recompiler/dispatcher, with a small ELF regression.
2. **Loop or dispatch budget:** inspect the original MIPS instructions, loaded pointers and MMIO register state; identify the actual producer/event. Never clear bits or increase budgets as a substitute for a missing transfer.
3. **Unsupported device/MMIO:** decode register width, transfer direction, source/destination extent, status/IRQ semantics, and the expected consumer. Add the smallest host-backed implementation.
4. **SIF/IOP wait:** confirm the IOP producer is executing, inspect DMA9/DMA10/FIFO, EE DMA5/6 and SMFLAG, and preserve actual interrupt-safe callback scheduling.
5. **Graphics:** inspect the GIF packet and GS state before implementing new GIF chain tags, GS registers, primitive rendering, or presentation.
6. **VU/VIF, input or audio:** implement only after guest execution reaches the corresponding real dependency.

**Acceptance:** a regression reproduces the blocker; the fix preserves state on rejection, never invents an IRQ or successful result, and the same real-game input advances beyond that exact condition.

## 6. Stage 3 — Incremental GIF, GS and visual presentation

Already implemented: normal GIF PATH3, GS register/VRAM sink, and source-chain END tag with inline payload. **Do not rewrite these.**

- If guest transfer evidence requires other source-chain tags (CNT/NEXT/REF/REFS/CALL/RET), implement their actual TADR/MADR/QWC/TIE semantics and appropriate guest-RAM validation. A detected tag is not by itself evidence that all chain modes are required.
- Continue decoding authentic GIF PACKED/REGLIST/IMAGE packets; apply only supported GS state/VRAM writes atomically. Invalid GS events must leave DMA pending and state uncommitted.
- When the game actually submits drawable primitives, implement the smallest native triangle/sprite/line path that consumes original GS state. Preserve scissor, texture/CLUT, blending and z-test behavior to the extent the observed packets demand it.
- Add a framebuffer-present path and frame/vblank scheduling once a real dependency or consistent scene output requires it. **A successful GIF tag count is not a rendered frame.**
- Prefer deterministic capture tests that compare GS registers, VRAM bytes, and eventual produced pixels against ground truth where available.

**Acceptance for the first visual milestone:** an original-game-derived primitive is rasterized from real GS packets into a framebuffer and can be inspected; no substitute textures or fabricated frame completion. A later milestone can require a sustained main-menu render.

## 7. Stage 4 — Menu, input, audio and gameplay

Progress through these only after boot and graphics have credible execution paths:

- Controller: map native gamepad state to the game's actual input polling/configuration flow; verify menu movement, selection and back.
- Audio: feed real game audio requests into a native backend, preserving guest timing and loop behavior as supported.
- Main menu: run the actual game state machine and show reproducible navigation and animation, not an authored imitation.
- Gameplay: load stages/characters, maintain timing and physics, render frames and accept player input; add continuous-session stability tests.

**Acceptance:** the original executable reaches and responds to at least one authentic main-menu interaction; later a playable match, documented per build.

## 8. Tests, CI and PR gates

For each milestone, require both **focused regressions** and (when the original media/toolchain are available) **a real SDBZ US replay**. Existing useful tests include:

- `tests/tools/test_game_runner.py` — linked native execution and auto-expansion.
- `tests/tools/test_function_pointer_tables.py` — guest-anchored callback table discovery.
- `tests/tools/test_chunked_dispatcher.py` — stable 256-function dispatch-cache ownership.
- `tests/smoke/ee_gs_state_test.cpp` and `tests/smoke/ee_gif_path3_test.cpp` — actual GIF/GS acceptance and rejection.
- Existing IOP/CDVD/SIF/DMA and EE/LLVM tests under `tests/smoke/` and `tests/tools/`.

Every runtime change must test the success path, malformed/unsupported input, bounds, reset/retry (when relevant), exact register changes, real data movement, and **non-completion on refusal**. Preserve compatibility across Windows and Linux.

GitHub gates before **squash merge**:

1. `Build and test`
2. `SIF DMA acceptance`
3. `OpenPS2BIOS ROM0 execution`

Create feature branches from up-to-date `main`, commit incremental source/tests, push a PR, monitor and repair all three checks, record the real-game acceptance evidence, then squash-merge only when the milestone is validated. Passing synthetic CI alone cannot prove original-game bootability.

## 9. Checkpoint record (update after each significant replay)

| Field | Verified checkpoint (2026-11-17) |
|---|---|
| Base commit | `d81a8e2ae5437434d485883ca2afd720e0e46152` |
| Latest merged change | PR #127 — GIF source-chain END |
| Original game | Super Dragon Ball Z (US), `SLUS_214.42` |
| IOP firmware | genuine OpenPS2BIOS v0.6 |
| Verified real-CDVD transfer | LBA 16, 261, 289 × 135, 450 × 4 |
| SIF/WaitSema | real DMA5 callback + `iSignalSema(8)` verified |
| GIF/GS | PATH3 normal and nine-QWC END chain accepted; VRAM transfer observed |
| Last focused replay | **12,168 EE dispatches** |
| Last guest PC | **`0x00102E24`** |
| Stop class | untranslated EE entry; **not boot complete** |
| Next action | restore verified translation provenance and compile/discover `0x00102E24` |

**Next developer instruction:** inspect the latest merged code and actual original-game inputs; begin with Stage 0 and Stage 1, then implement the next concrete blocker. Do not stop at a high-level proposal. Keep this file updated with each merged milestone and the exact proof that advanced the game.