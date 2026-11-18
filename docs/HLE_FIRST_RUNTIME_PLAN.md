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

## Primary media source: extracted disc folder

The normal SDBZ native runner should operate directly on an **extracted disc
directory**, not require either an ISO file or a ROM0 image. This follows
[the SDBZ fork](https://github.com/twist84/ps2-web-recomp/tree/SDBZ),
especially `runtime/src/disc_folder.cpp` and
`docs/sdbz-runtime-disc-layout.md`:

- Traverse the extracted directory with safe path resolution; resolve
  `cdrom0:` requests case-insensitively to the real files.
- When a guest uses ISO directory search or LBA-based CDVD reads, construct
  a **deterministic virtual ISO9660 index in memory** from the folder:
  primary volume descriptor, path tables, directory entries, file extents and
  sector reads. Do not write or require `.ps2-disc` sidecars.
- The folder layout's virtual LBAs are not the original physical disc's LBAs.
  Ordinary `sceCdSearchFile`-based code discovers the new extents correctly;
  hardcoded absolute LBAs need their own compatibility work.
- The fork's `GAME/` direct-native ARKD path is separate from classic
  `ARKD_DVD.IRX` / `INFO.DAT` / `GAME.DAT` behavior. Preserve both
  boundaries and keep guest-visible bytes distinct from native `NATIVE/`
  texture/audio replacements.
- Do not silently inject `INFO.DAT`, regenerate archive contents or fake
  sector replies just to pass a boot check. Unknown requests fail closed.
- Retain raw ISO and explicit OpenPS2BIOS options as optional reference /
  compatibility modes, not prerequisites.

**Current first slice:** `--disc-folder=DIR` mounts a validated directory,
constructs the existing HLE IOP service bridge without a ROM0, and passes
guest `LOADFILE` requests to the existing `c_disc_loader`.
The folder-backed `c_folder_sector_source` now synthesizes an ISO9660
primary volume descriptor, path tables, directories and deterministic virtual
sector extents in memory, and is wired to authentic IOP CDVD DMA3 reads.
Its `find_file` API returns file LBAs and sizes, but no IOP HLE
`sceCdSearchFile` guest service is yet registered. Full IOP kernel boot
services, GAME archive virtualization and any ARKD bypass remain future work;
no playable boot is implied.

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

1. `--disc-folder=DIR` provides a no-ROM0 directory-backed `LOADFILE`
   foundation, but a complete no-BIOS boot/service lifecycle remains missing.
2. The existing `c_disc_loader` resolves `cdrom0:` IRX requests from an
   extracted directory safely. It is not a general ISO9660 directory service.
3. The extracted-folder virtual ISO9660 `c_sector_source` and CDVD DMA3
   wiring exist. The guest-facing `sceCdSearchFile` service and remaining
   CDVD library imports are not yet implemented. No INFO/GAME synthesis or
   unrelated native asset/resource-pack features are ported.
4. The non-ROM0 SIFCMD/SIFMAN compatibility path lacks a complete HLE kernel
   reset, service library registry, scheduler and game-driven IRX lifecycle.
5. Guest modules such as `ARKD_DVD.IRX` may use `sceCdSearchFile` to locate
   INFO/GAME archives; implement and test lookup using the synthetic index.
6. The legacy `SDBZ_IOP_ROM0` variable remains implicit for normal ISO
   runs, but is ignored when `--disc-folder` explicitly selects HLE.
   Future `--iop-boot=hle|rom0` should make both modes unambiguous.
7. Existing `--hle-startup` covers part of the EE HLE path. Audit additional
   observed kernel operations before claiming a default HLE game boot.

## Implementation order

### PR 1 — Folder-backed bootstrap and no-BIOS kernel foundation

- **First slice committed:** `--disc-folder=DIR` for HLE IRX `LOADFILE`,
  including Python launcher forwarding, validation and integration coverage.
  Explicit folder selection overrides the legacy ROM0 environment variable.
- Continue toward an explicit `--iop-boot=hle|rom0` policy. Make HLE the
  default game flow while retaining `rom0` for comparisons.
- Keep extracted-folder file resolution independent of the ROM0 sector
  backend. Port virtual sector ownership only when CDVD LBA reads or
  `sceCdSearchFile` require it.
- Bootstrap IOP kernel services with a well-defined reset state, library
  registration and pending request/event state; do not start an imaginary
  R3000A ROM0 instruction stream.
- Reject conflicting ROM0/IRX/mode options explicitly, and keep legacy
  ROM0 diagnostics working via explicit selection.
- Add synthetic integration tests that run without any BIOS file, validate
  service initialization/guest-visible state, and fail closed on unknown
  imports and invalid SIF transfers.

### PR 2 — Virtual folder ISO9660 lookup and executable game IRX

- Port the SDBZ fork's virtual ISO9660 directory/LBA layer as a
  `c_sector_source` backed by extracted files. Preserve case/version
  matching, enforce bounds and reject ambiguous/invalid paths. Do not
  rely on fixed physical LBAs or scan ELF strings to force-load modules.
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

- Run the original US ELF from the extracted disc folder, with **no ISO and
  no ROM0**, through `tools/run_game.py --disc-folder=...` and a Windows
  batch helper. Preserve original filenames and bytes.
- Validate all IRX and SIF services actually reached, folder file hashes,
  virtual sector/lookup semantics, guest callback order and native dispatch.
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

## Immediate follow-up

Implement the guest-facing `sceCdSearchFile` and CDVD HLE library service
against the synthetic folder-sector index, with negative path/lookup tests.
Then wire the general HLE IOP kernel bootstrap and game-requested IRX modules. Keep the original 8,046
LLVM guest modules unchanged until a genuine guest translation change is needed.
