# HLE-first native PS2 runtime: boot and service contract

Status: architecture decision and migration plan (2026-11-18). This document
does **not** claim the HLE-first path already boots Super Dragon Ball Z.

## Decision

Follow the high-level emulation boundary demonstrated by
[snuri00/ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp):

- **EE game code:** compile the original R5900 instructions to LLVM IR and host
  native code. Preserve exact architectural guest behavior.
- **EE BIOS/kernel services:** HLE syscall and kernel bookkeeping, including
  threads, semaphores, interrupt handlers, timers and SIF entry points. Deliver
  real guest callbacks and maintain consistent guest-visible memory and MMIO.
- **IOP BIOS/kernel services:** HLE for operating-system services and import
  libraries (e.g. sysmem, loadcore, intrman, thbase, thsemap, modload, sifman,
  sifcmd). These services must have tested semantics and state, not unconditional
  success-return stubs.
- **Game-provided IRX modules:** load the actual modules, relocate them, resolve
  their exports/imports and execute their R3000A guest code with the existing
  interpreter. Keep real disc assets and driver requests.
- **Hardware:** faithfully model guest-visible EE/IOP DMA, SIF transport,
  CDVD, interrupts, GIF/VIF/VU/GS and later SPU2/IPU. Hardware event delivery is
  scheduled and tested; it is not arbitrary syscall success or synthetic state.
- **OpenPS2BIOS ROM0:** retain as an explicitly selected reference/integration
  path for comparisons and specialized tests. A BIOS image must not be required
  by the normal HLE-first game runner.

The ps2-web-recomp implementation is a useful architectural reference, not a
drop-in PS2 BIOS specification. Validate ports against guest traces and tests.

## Existing implementation to preserve

- `src/tools/ee_game_runner_main.cpp`: selected EE startup/syscall/thread/
  semaphore/interrupt HLE; authentic native EE dispatch and hardware state.
- `src/iop/iop_cpu.cpp` and `src/iop/iop_kernel.cpp`: R3000A interpreter,
  IRX module loader, import resolution and guest module threads.
- `src/iop/iop_sifman_bridge.cpp`, `iop_sifcmd_bridge.cpp`,
  `iop_sif_rpc_bridge.cpp`, `iop_disc_loader.cpp`: existing partial
  kernel/service-HLE-to-real-IRX path.
- `src/iop/iop_cdvd.cpp` and `src/iop/iop_sif[01]_dma_transport.cpp`:
  genuine sector/transfer and hardware bridge work already validated with ROM0.
- `src/runtime/ee_scanout_clock.cpp`: separate cycle-input scanout state
  model, **not connected** until native EE timing is validated.
- The 8,046-module native translation cache and provenance validation. Switching
  an IOP boot backend should not require regenerating unmodified EE LLVM IR.

## Known source-level obstacles

1. The game runner explicitly rejects `--disc-image` unless an IOP ROM0 is
   supplied (`disc_image_path != nullptr && iop_rom0_path == nullptr`).
2. The IOP kernel is constructed only for an explicit IRX list or ROM0 image,
   so there is no standalone HLE kernel bootstrap yet.
3. The CDVD ISO-sector source and DMA3 transport are currently created only
   inside the ROM0 boot branch; they need backend-independent ownership.
4. The non-ROM0 SIFCMD/SIFMAN HLE compatibility path exists, but its
   initialization is coupled to explicit guest IRX startup. It lacks a full
   general boot-service provider and an ISO-backed LOADFILE/module path.
5. `iop_disc_loader.cpp` currently resolves `cdrom0:` paths under a loose
   host directory. A normal HLE bootstrap from `--disc-image=ISO` needs
   directory records from that mounted image, with sector reads validated
   against original disc bytes, not guessed host paths.
6. `SDBZ_IOP_ROM0` currently implicitly activates ROM0 in the runner.
   Backend selection must be explicit and unambiguous to avoid silently
   changing execution mode during the transition.
7. Existing `--hle-startup` enables only part of the EE HLE path. Audit which
   game-observed kernel operations still depend on guest BIOS assumptions.

## Implementation order

### PR 1 — Explicit boot backend and no-BIOS kernel foundation

- Introduce a documented boot policy, e.g. `--iop-boot=hle|rom0`. The new
  default game flow is HLE, while `rom0` remains supported for comparison.
- Extract ISO-sector ownership and IOP/SIF setup from the ROM0-only branch.
  Remove the `--disc-image`/ROM0 coupling only after there is a real HLE
  startup path to consume the image.
- Bootstrap IOP kernel services with a well-defined reset state, library
  registration and pending request/event state; do not start an imaginary
  R3000A ROM0 instruction stream.
- Reject conflicting ROM0/IRX/mode options explicitly, and keep legacy
  ROM0 diagnostics working via explicit selection.
- Add synthetic integration tests that run without any BIOS file, validate
  service initialization/guest-visible state, and fail closed on unknown
  imports and invalid SIF transfers.

### PR 2 — ISO-backed LOADFILE and executable game IRX

- Resolve guest `cdrom0:` paths using actual ISO9660 directory records.
  Preserve case/version matching semantics, enforce bounds and reject
  ambiguous/invalid paths; do not scan arbitrary ELF strings to force-load.
- Load relocated guest IRX only after a real EE LOADFILE/RPC request; bind
  imports to either registered game providers or individually implemented
  HLE kernel libraries.
- Run game-supplied IRX entry points and module threads in the existing R3000A
  interpreter. Keep scheduling and error codes observable and deterministic.

### PR 3 — SIF/RPC and asynchronous kernel contracts

- Test SIFCMD/SIFMAN request/reply order, EE/IOP memory copies, DMA
  acknowledgements and timeout/error paths against grounded reference behavior.
- Implement missing EE/IOP HLE APIs in small groups, preferring guest-observed
  requests. Do not report success for unknown imports.
- Advance IOP work on explicit scheduler events, not host wall time; ensure
  an HLE RPC response obeys blocking and callback semantics.

### PR 4 — SDBZ no-ROM0 boot acceptance

- Run the original US ELF and ISO with **no** `SDBZ_IOP_ROM0` and no alternate
  hidden BIOS input, using existing `tools/run_game.py`/Windows batch helpers.
- Validate all IRX and SIF services actually reached, original disc-sector
  hashes, guest callback order and native dispatch.
- Compare observed checkpoints with the historical ROM0-assisted
  `12,232`-dispatch VBlank wait. Do not demand equal dispatch counts across
  different kernel implementations; demand equal meaningful guest semantics.
- A no-BIOS boot reaching `0x00175210` still is **not** complete boot or a
  rendered frame. Independent EE cycle timing and authentic VBlank scheduling
  remain necessary for that next dependency.

## Non-goals and regression gates

- No wholesale conversion of guest game or game-supplied IRX code into HLE.
- No unconditional success stubs, fabricated SIF-ready flags, DMA completions,
  interrupt causes, EE cycles, VBlank or framebuffer.
- Preserve existing ROM0 and SIF/DMA CTests as an optional differential path.
  Add separate default-HLE CI so the no-BIOS path cannot regress unnoticed.
- Test on synthetic redistributable fixtures in CI; never commit the original
  SDBZ ISO, ELF, a ROM0 binary or generated copyrighted code.
- Do not treat the external reference's HLE timing constants as independently
  verified PS2 hardware timing without differential validation.

## Immediate first commit

This plan and a cross-reference from `SDBZ_NATIVE_BOOT_PLAN.md` establish the
backend boundary before changing executable behavior. Follow with a discrete
feature commit introducing the boot selector, HLE service registration and
basic no-ROM0 synthetic startup tests.
