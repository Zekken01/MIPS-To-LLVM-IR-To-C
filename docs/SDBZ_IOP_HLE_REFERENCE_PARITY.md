# SDBZ USA IOP HLE — ps2-web-recomp parity audit

## Baseline and scope

Main architectural reference: [twist84/ps2-web-recomp, `SDBZ` branch](https://github.com/twist84/ps2-web-recomp/tree/SDBZ), specifically [`runtime/src/iop/libs.cpp`](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/libs.cpp), blob `061b8a092688660005764d9d45d9fb1f348546df`.

Current native LLVM-IR implementation: [`src/iop/iop_kernel_hle_bridge.cpp`](../src/iop/iop_kernel_hle_bridge.cpp), audited blob `623875f9bacb21633a539ce2529570b81132c474`.

The reference file defines **172 explicitly named `def("library", ordinal, ...)` handlers**. The HLE-first runtime now installs **177 guest-callable export ordinals** across
the kernel bridge (146), CDVDMAN bridge (7), SIFCMD bridge (11) and
SIFMAN bridge (13). **131** of the kernel bridge exports match explicitly
named `libs.cpp` reference definitions. The earlier 152 count omitted
seven pre-existing registrations in separate device/SIF bridges. Counts include no claim about full behavioral equivalence. The reference also uses loops/dynamic registrations (e.g. `secrman#4/#5`) and separate IOP modules (`io.cpp`, `sif.cpp`, `cdvd.cpp`, `spu2.cpp`) and EE kernel syscalls. The full reference runtime is larger than these 172 direct definitions.

Never bulk-register an unimplemented opcode. Each `k_exports` entry installs a **real executable import stub**, so an unimplemented handler must remain unresolved instead of returning fabricated success.

## Explicit `libs.cpp` IOP service comparison

| Service library | Registered / reference explicit | Missing from this bridge |
|---|---:|---|
| `sysmem` | 8/8 | — |
| `loadcore` | 4/6 | `12 QueryBootMode`, `27 SetRebootTimeLibraryHandlingMode` |
| `intrman` | 9/9 | — |
| `thbase` | 25/37 | `12 DisableDispatchThread`, `13 EnableDispatchThread`, `16 RotateThreadReadyQueue`, `17 iRotateThreadReadyQueue`, `18 ReleaseWaitThread`, `19 iReleaseWaitThread`, `21 CheckThreadStack`, `29 SuspendThread`, `31 ResumeThread`, `36 iSetAlarm`, `38 iCancelAlarm`, `41 GetSystemStatusFlag` |
| `thsemap` | 8/8 | — |
| `thevent` | 11/11 | — |
| `thmsgbx` | 6/6 | — |
| `thfpool` | 6/6 | — |
| `thvpool` | 8/8 | — |
| `sysclib` | 27/31 | `4 setjmp`, `5 longjmp`, `19 sprintf`, `42 vsprintf` |
| `stdio` | 3/3 | — |
| `modload` | 6/8 | `7 LoadStartModule`, `8 StartModule` |
| `vblank` | 0/6 | `4 WaitVblankStart`, `5 WaitVblankEnd`, `6 WaitVblank`, `7 WaitNonVblank`, `8 RegisterVblankHandler`, `9 ReleaseVblankHandler` |
| `timrman` | 0/13 | `4 AllocHardTimer`, `5 ReferHardTimer`, `6 FreeHardTimer`, `7 SetTimerMode`, `8 GetTimerStatus`, `9 SetTimerCounter`, `10 GetTimerCounter`, `11 SetTimerCompare`, `16 GetHardTimerIntrCode`, `20 SetTimerHandler`, `22 SetupHardTimer`, `23 StartHardTimer`, `24 StopHardTimer` |
| `dmacman` | 5/7 | `16 dmac_set_dpcr2`, `17 dmac_get_dpcr2` |
| `heaplib` | 4/4 | — |
| `secrman` | 1/1 | — (explicit `#4/#5` loop definitions are safe failures) |

## Implemented this Windows-log-driven batch

- Guest-thread scheduling: `thbase` thread creation/start/termination/deletion, `StartThreadArgs`, `ExitDeleteThread`, modeled sleep/delay/wakeup counts, priority update, guest-readable thread status, virtual system time, and guest timer callbacks.
- Synchronization: real stateful `thevent` and `thsemap` create/delete/wait/poll/status paths, waiter wakeups, bounded guest-memory output.
- Message/pools: guest-owned mailbox queues, `heaplib` allocations, fixed and variable pool block ownership, allocation/poll/free/status paths, and delayed waiters.
- Memory/string services: `sysmem` actual allocation ownership and size queries; bounded `sysclib` parsing, tokenization, memory/word operations; stdio guest debug output.
- Interrupt/cache state: IOP interrupt mask and coherent direct RAM cache-flush operations; neither synthesizes a DMA/IRQ completion.

Tests cover the new imports through guest R3000 stubs, plus conservative rejection of unknown library/ordinal pairs. Existing EE/IOP hardware and kernel bridges remain separate.

## Stopgap implementations to avoid

Some reference HLE methods return hard-coded successful values or simulate timing without calibrated hardware. These are not evidence of a complete guest-visible service. In this project, keep unsupported hardware operations fail-closed: do not invent `secrman` card authentication, DMA transfer completion, vblank events, actual IOP TIMER interrupts, `modload` success, SIO2 controller responses or textured GS output.

## Next coherent parity batches

1. **IOP module lifecycle**: `modload` guest module registry/load/start/wait/search, exact LOADFILE/R3000 interactions; review installed exports before missing imports in `MCMAN/MCSERV/PADMAN`.
2. **Synchronization and PS2SDK system**: remaining `thbase` priority/dispatch/release-wait variants, `sysclib` nonlocal jump and bounded formatted output; rigorously test guest CPU control flow.
3. **Timing**: `timrman`, IOP vblank, interrupt-context alarm callbacks, and EE/IOP clock calibration. Current timer scheduling remains an explicitly modeled guest-instruction clock, not hardware-certified timing.
4. **Device services**: `secrman`, `ioman`, `libsd`, `cdvdman`, `sifman/sifcmd`, `sio2man`; reconcile existing runtime bridges and actual IRX exports instead of assuming all belong to `k_exports`.

The 2026-11-19 22:41 Windows evidence had a final committed `MCMAN.IRX` unresolved import `thbase#33` after the prior `sysclib#12` blocker cleared, with 14,385 final EE dispatches. That old log **does not validate any HLE changes made afterwards**. Newly matched Windows logs must confirm `MCMAN` linking and its entrypoint behavior. CLI compilation progress is never evidence of guest boot success.

## Acceptance protocol

For each batch: correct guest ABI and pointer bounds; tested state mutations; unknown import failure preserved; IRX loader atomic on rejection; complete CI at exact new branch HEAD; only then a matched Windows replay can establish actual game progress. CI green alone never establishes playable boot.

## 2026-11-19 23:30 — MODLOAD guest registry and buffer batch

The matched Windows replay again stopped at **14,385 dispatches**, with a
committed LOADFILE request for `MCMAN.IRX` failing on
`modload#13` (the preceding `thbase#33` dependency was cleared).
The diagnostic run has zero newly discovered functions, and all three
GS snapshots are black.

Added six R3000-callable `modload` exports, based on
`ps2-web-recomp/runtime/src/iop/libs.cpp`:

- `#13 SetCheckKelfPathCallback`: records a validated guest callback
  pointer, but **does not verify KELF files, execute callbacks, or
  authorize authentication**. Unsupported KELF loading remains rejected.
- `#10 LoadModuleBuffer`: resolves guest ELF/IRX bytes into the
  existing transactional IOP linker and publishes a real module ID
  only after successful linking. Does not start the guest module.
- `#16 GetModuleIdList`, `#18 GetModuleIdListByName`,
  `#17 ReferModuleStatus` and `#22 SearchModuleByName`:
  report only actual linked modules, with bounded guest output buffers,
  case-insensitive name search, and unknown-module errors.

`#7 LoadStartModule` and `#8 StartModule` remain deliberately
unregistered: guest module start/wait completion and caller unblock
must be implemented coherently before exposing them. This batch does
not claim that MCMAN's entrypoint runs or that EE execution progresses.
The new HLE implementations are exercised through R3000 import stubs.

The preceding 48 explicit reference gaps are reduced to **42**
by adding six implementations. The new Windows boot must validate
`MCMAN.IRX` and any subsequent imports; static/cached IRX data
are not proof of successful module execution.

## 2026-11-19 23:41 — IOMAN CD-ROM + device/security boundary batch

The matched ZIP contains five artifacts and three black PPM captures.
Final replay is **14,385 dispatches**, with zero newly discovered IR
functions. `SIO2MAN.IRX` started; `MCMAN.IRX` failed on
`ioman#20` (the preceding `modload#13` dependency cleared).
The source ELF SHA-256 and disc root match the previous run.

Using `ps2-web-recomp/runtime/src/iop/io.cpp` and the PS2SDK
`ioman.h` ABI, the native bridge now provides a read-only
`c_folder_sector_source`-backed `ioman` subset:

- `#4 open`, `#5 close`, `#6 read`, `#8 lseek`:
  per-fd position/ownership, sector-aligned reads, bounded IOP RAM,
  correct failure on invalid fds and unsupported writes.
- `#7 write`: only TTY stdout/stderr via the existing debug console;
  CD-ROM writes and unknown devices are rejected.
- `#16 getstat`: actual on-disc file mode/size in the bounded
  40-byte guest structure.
- `#20 AddDrv`, `#21 DelDrv`: **documented failed HLE boundary**.
  The reference itself returns `-1` for these operations, and
  the native runtime does not synchronously execute guest
  device-init/deinit callbacks. Driver registration or memory-card
  readiness is never fabricated.
- `secrman#4/#5/#6`: explicitly *unsuccessful* security boundaries;
  SecrAuthCard returns false instead of pretending a card was authenticated.

Only these 11 new ordinals were registered. Other unsupported IOMAN
operations remain unresolved. The real CD-ROM source is attached only
for the existing validated extracted-disc boot mode, and all
IO descriptors reset on IOP reboot. Guest R3000 tests exercise file
open/read/seek/stat/close, invalid operations, rejected driver
registration and denied security calls.

The explicit `libs.cpp` count becomes **131/172**, with **41**
remaining gaps; the `ioman` reference lives in `io.cpp` and is not
part of the 172 explicitly named `libs.cpp` handlers.

**Next Windows validation:** MCMAN link/start outcome, subsequent
imports, EE dispatches beyond 14,385, and authentic GS framebuffer.
CI passing alone does not establish this.

## 2026-11-19 — CDVDMAN clock and mounted-disc state batch

The next matched Windows ZIP again stopped at **14,385 EE dispatches**,
with one successful `SIO2MAN.IRX` load/start and a new rejected
`MCMAN.IRX` link due to **`cdvdman#24`**. The preceding
`ioman#20` import was cleared. The final replay generated zero new
EE function modules; its three GS images were all black.

The existing extracted-disc-only `c_cdvdman_bridge` now includes
guest R3000 syscall stubs for `#4 sceCdInit`, `#8 sceCdGetError`,
`#12 sceCdGetDiskType`, `#13 sceCdDiskReady`,
`#24 sceCdReadClock`, `#28 sceCdStatus`, in addition to
existing `#10 sceCdSearchFile`. These are grounded in the validated
mounted virtual DVD and (for the RTC) host UTC calendar converted
to the reference's JST BCD layout. The 8-byte clock output is
guest-memory checked before writing.

The subset does not implement CDVDMAN asynchronous reads,
real seek timing, callbacks, or interrupt completion, and does not
register those unsupported imports. The mounted-media ready status
reflects only the virtual disc backend, not a physical PS2 drive.
Guest R3000 tests verify CDVDMAN clock imports and media queries,
unknown ordinal rejection, invalid memory rejection and original
search-file semantics.

The **172-handler libs.cpp reference total is unchanged**: these
CDVDMAN handlers live in `runtime/src/iop/cdvd.cpp`. The native
HLE ordinal total increases from **146 to 152**. The 41 explicit
`libs.cpp` parity gaps remain separate. The next matched real
Windows boot must confirm if MCMAN links/starts and whether EE
execution or framebuffer contents progress.

## 2026-11-20 — MCSERV SIFMAN initialization / mailbox batch

Matched archive `ps2-game-hle(2).zip` (five diagnostic files plus three
black PPMs) uses the same USA `SLUS_214.42` SHA-256 and disc root as
the previous Windows run. **Final replay 14,523 EE dispatches (+138)**
and zero new EE IR functions, with 3,524 cached IR files.
`SIO2MAN.IRX` linked/started (module 1/thread 1), then
`MCMAN.IRX` linked/started (module 2/thread 3), and an authentic
`MCSERV.IRX` LOADFILE request failed on **`sifman#5`**,
`sceSifInit`. The failing MCSERV request had no module ID or
entrypoint thread. The EE ended via `KExit` at EPC `0x001748C4`;
all GS display, upload and native WebGPU snapshots are black.

Using `ps2-web-recomp/runtime/src/iop/sif.cpp` as primary reference
and cross-checking PS2SDK `iop/system/sifman/src/sifman.c`, the
existing SIFMAN bridge now registers **13 ordinals**:
`#4 sceSifDma2Init`, `#5 sceSifInit`, `#6 sceSifSetDChain`,
`#7 sceSifSetDma`, `#8 sceSifDmaStat`,
`#21/#22 Get/SetMSFlag`, `#23/#24 Get/SetSMFlag`,
`#25/#26 GetMainAddr/GetSubAddr`, `#27 SetSubAddr`,
`#29 sceSifCheckInit`.

`#4/#5` configure actual guest-visible IOP DMA priority and channel
control registers, with reinitialization state owned by the SIFMAN
bridge. `#6` initializes the SIF1 DMA chain's block-count register
but **does not start a phantom DMA transfer**. `#8` reports no
in-flight DMA for the bridge's synchronous `#7` transfer model.
`#29` reports whether guest `sceSifInit` actually executed.
Flag and address operations use the shared EE/IOP SIF mailbox model,
preserve producer/acknowledger ownership, and validate guest
subaddresses. Installed import stubs do not set SMFLAG, fabricate
BOOTEND or complete an RPC. SIFMAN interrupt handlers and asynchronous
transfer callbacks remain unresolved.

Tests exercise genuine R3000 guest `#5` execution, IOP DMA register
effects, reinitialization/reset semantics, mailbox register ownership,
SIF status and deliberate failures on unimplemented imports and
invalid guest pointers.

**Next real boot:** determine whether `MCSERV.IRX` now links, and
diagnose any subsequent `sifcmd` RPC or guest `mcman` dependency
rather than bulk registering stubs. The preceding +138 dispatches
do not prove video output or fully initialized memory-card hardware.

## 2026-11-20 — MCSERV guest SIFCMD RPC server batch

The next matched Windows archive, `ps2-game-hle(3).zip`, retains the
correct USA ELF SHA-256 and disc folder. It contains one final replay
with **14,523 EE dispatches** (no change from the previous batch),
3,524 cached IR modules, and **zero newly generated EE functions**.
Two LOADFILE operations succeeded and started guest R3000 module threads:
`SIO2MAN.IRX` (module 1/thread 1) and `MCMAN.IRX` (module 2/thread 3).
The real `MCSERV.IRX` attempt failed linking on `sifcmd#14`
(`sceSifInitRpc`), rather than the previous `sifman#5`.
All three GS diagnostic images remain completely black; terminal
EE syscall is still `KExit` at EPC `0x001748C4`.

Based on `ps2-web-recomp/runtime/src/iop/sif.cpp`, the IOP SIFCMD
bridge now registers a **coherent eight-ordinal guest RPC family**:

- `#14 sceSifInitRpc`: idempotent guest-side RPC initialization
  after SIFCMD is actually initialized, never by registering imports;
- `#17 sceSifRegisterRpc`, `#19 sceSifSetRpcQueue`,
  `#24 sceSifRemoveRpc`, `#25 sceSifRemoveRpcQueue`: validated
  guest server/function/buffer/queue ownership and lifecycle;
- `#22 sceSifRpcLoop`: the real IOP guest thread blocks pending
  requests, wakes only on an actual accepted EE SIF1 RPC call, runs
  its registered R3000 callback, and returns through a dedicated
  completion trampoline;
- `#18 sceSifCheckStatRpc`: reads actual client-busy status from
  validated IOP memory;
- `#23 sceSifGetOtherData`: bounded EE RAM to IOP RAM copy with
  explicit backend and address validation.

The existing SIFRPC bridge recognizes **only registered guest servers**.
An incoming EE bind can be accepted after the guest server exists;
incoming calls are validated and staged, and **no EE output or SIF0
reply is produced until the guest handler executes**. The genuine
R3000 callback's return pointer determines reply data, with bounded
IOP and EE RAM, and the SIF0 response uses the pre-existing armed
DMAC transport. All guest server/pending-call state resets on IOP reboot.

A focused regression executes guest R3000 `sceSifInitRpc`, blocks a
real RPC-loop guest thread, supplies an EE SIF1 bind/call, verifies
that no premature reply is sent, checks an actual guest callback's
write to the response buffer, and verifies the outgoing SIF0 reply.
Unimplemented `sifcmd#12/#13/#15/#16/#28` remain unregistered:
real command-send, IOP-to-EE client RPC, and callback interrupt
semantics must not be fabricated. The current MCSERV static
`sifcmd` import set (`#14/#17/#19/#22/#23/#24/#25`) is now
registered, but **only a subsequent matched Windows boot** can prove
that MCSERV links and its guest thread starts or runs correctly.

The native HLE ordinal total is **177**, including bridge exports
defined outside `libs.cpp`. The explicit `libs.cpp` match count
remains **131/172**.
