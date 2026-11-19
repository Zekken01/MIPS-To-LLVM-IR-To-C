# SDBZ USA IOP HLE — ps2-web-recomp parity audit

## Baseline and scope

Main architectural reference: [twist84/ps2-web-recomp, `SDBZ` branch](https://github.com/twist84/ps2-web-recomp/tree/SDBZ), specifically [`runtime/src/iop/libs.cpp`](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/libs.cpp), blob `061b8a092688660005764d9d45d9fb1f348546df`.

Current native LLVM-IR implementation: [`src/iop/iop_kernel_hle_bridge.cpp`](../src/iop/iop_kernel_hle_bridge.cpp), audited blob `623875f9bacb21633a539ce2529570b81132c474`.

The reference file defines **172 explicitly named `def("library", ordinal, ...)` handlers**. Our bridge registers **129 actual guest-callable HLE ordinals**; **124 match explicitly named reference definitions**. Counts include no claim about full behavioral equivalence. The reference also uses loops/dynamic registrations (e.g. `secrman#4/#5`) and separate IOP modules (`io.cpp`, `sif.cpp`, `cdvd.cpp`, `spu2.cpp`) and EE kernel syscalls. The full reference runtime is larger than these 172 direct definitions.

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
| `modload` | 0/8 | `7 LoadStartModule`, `8 StartModule`, `10 LoadModuleBuffer`, `13 SetCheckKelfPathCallback`, `16 GetModuleIdList`, `17 ReferModuleStatus`, `18 GetModuleIdListByName`, `22 SearchModuleByName` |
| `vblank` | 0/6 | `4 WaitVblankStart`, `5 WaitVblankEnd`, `6 WaitVblank`, `7 WaitNonVblank`, `8 RegisterVblankHandler`, `9 ReleaseVblankHandler` |
| `timrman` | 0/13 | `4 AllocHardTimer`, `5 ReferHardTimer`, `6 FreeHardTimer`, `7 SetTimerMode`, `8 GetTimerStatus`, `9 SetTimerCounter`, `10 GetTimerCounter`, `11 SetTimerCompare`, `16 GetHardTimerIntrCode`, `20 SetTimerHandler`, `22 SetupHardTimer`, `23 StartHardTimer`, `24 StopHardTimer` |
| `dmacman` | 5/7 | `16 dmac_set_dpcr2`, `17 dmac_get_dpcr2` |
| `heaplib` | 4/4 | — |
| `secrman` | 0/1 | `6 SecrAuthCard` |

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
