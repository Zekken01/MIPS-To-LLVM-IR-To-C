# SDBZ USA IOP HLE — ps2-web-recomp parity audit

## Baseline and scope

Main architectural reference: [twist84/ps2-web-recomp, `SDBZ` branch](https://github.com/twist84/ps2-web-recomp/tree/SDBZ), specifically [`runtime/src/iop/libs.cpp`](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/libs.cpp), blob `061b8a092688660005764d9d45d9fb1f348546df`.

Current native LLVM-IR implementation: [`src/iop/iop_kernel_hle_bridge.cpp`](../src/iop/iop_kernel_hle_bridge.cpp); the baseline audited blob was `623875f9bacb21633a539ce2529570b81132c474`.

The reference file defines **172 explicitly named `def("library", ordinal, ...)` handlers**. The HLE-first runtime now installs **196 guest-callable export ordinals** across
the kernel bridge (154), CDVDMAN bridge (14), SIFCMD bridge (15) and
SIFMAN bridge (13). **137** of the kernel bridge exports match explicitly
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
| `sysclib` | 29/31 | `4 setjmp`, `5 longjmp` |
| `stdio` | 3/3 | — |
| `modload` | 6/8 | `7 LoadStartModule`, `8 StartModule` |
| `vblank` | 6/6 | — |
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
2. **Synchronization and PS2SDK system**: remaining `thbase` priority/dispatch/release-wait variants, `sysclib` nonlocal jump and extended formatted-output parity; rigorously test guest CPU control flow.
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

## 2026-11-20 — LOADFILE module startup argument ABI after LIBSD link

Matched Windows archive `ps2-game-hle(4).zip` has all five expected
diagnostics and three black GS PPMs, with the unchanged USA
`SLUS_214.42` SHA-256 and extracted-disc root. The **final**
committed SIFRPC evidence now shows four successful IRX load/start
attempts: `SIO2MAN.IRX` (module 1/thread 1), `MCMAN.IRX`
(2/3), `MCSERV.IRX` (3/4) and `LIBSD.IRX` (4/5). This
proves the prior `sifcmd#14` linker dependency cleared and four
entrypoint threads were started, **not** that their callbacks or
device initialization completed.

After the fourth module, the next genuine EE SIF1 transaction is
a 512-byte method-0 LOADFILE RPC to server `0x80000006`, with a
64-byte command, `recv_size=8` and record ID 5. It fails closed
as `invalid_arguments`, rather than reporting another IRX load.
The previous parser rejected every nonzero argument length, although
PS2SDK's real `_lf_module_load_arg` defines a bounded
`arg_len` (word 0), 252-byte path at byte 8, and 252-byte
NUL-separated module arguments at byte 260. The raw payload bytes
are not in this log, so the precise argument values must be
confirmed by the next real boot, not inferred from the RPC header.

The loader now validates `arg_len <= 252`, verifies the
complete 512-byte DMA payload for nonempty arguments, ensures
each supplied argument is NUL-terminated, and retains the exact
bytes for guest startup. `c_disc_loader` passes these arguments
to the linked module's genuine guest `_start(argc, argv)` ABI.
`c_kernel` stores `argv[0]` (guest module path), all subsequent
arguments, and a null pointer terminator inside the **thread-owned
bounded IOP stack**, rather than passing host pointers or silently
dropping arguments. This preserves guest-visible argument lifetime
and keeps the guest stack pointer below its initial argv storage.

Regression tests cover the guest argv pointers/strings, a
PS2SDK-shaped 512-byte SIF1 LOADFILE packet with nonempty
NUL-delimited argument list, and rejection of oversized or
unterminated argument lists. Missing/invalid modules still fail
closed; there is no synthetic memory-card initialization or
successful startup result.

The final replay ended at **11,043 EE dispatches**, with
`KExit` EPC `0x00175064`; the expansion pass discovered two
additional EE functions and the translation cache now has **3,526**
IR entries. The lower dispatch count compared to the prior 14,523
is a different stop point, not evidence that four IRX modules
failed to link. GS display, upload and WebGPU images remain black.

Next Windows batch should confirm whether the new argument-bearing
request is accepted, the exact next requested IRX path, actual
IOP guest execution, and whether any new unresolved import,
runtime service or GS producer becomes the next blocker.

## 2026-11-20 00:47 — CRI_ADXI inbound import / reverse SIFRPC transport

The matched `ps2-game-hle(5).zip` contains five diagnostics and
three solid-black GS PPMs. Same USA ELF SHA-256 and mounted disc
root as the last boot, **14,805 final EE dispatches** (previously
11,043), with **zero new discovered EE functions** and 3,526 cached
IR modules. Genuine committed LOADFILE RPC sequence 17 now attempts
`cdrom0:\\CRI_ADXI.IRX;1`: `load_status=7`,
`link_status=5`, `module=thread=0`, first unresolved
import **`sifcmd#16`**. Earlier `SIO2MAN`, `MCMAN`,
`MCSERV` and `LIBSD` remain linked/started. This is module
thread creation evidence, not proof of their successful
initialization. The EE terminates with `KExit` syscall at
`EPC=0x001748C4`; no nonblack GS pixels.

`sifcmd#16` is **`sceSifCallRpc`**, an IOP-originated call
to an EE RPC server, not another EE-to-IOP LOADFILE operation.
The paired `#15 sceSifBindRpc` is implemented as a coherent
reverse-RPC client lifecycle using
`ps2-web-recomp/runtime/src/iop/sif.cpp` and PS2SDK's
`sifrpc-common.h` client-packet ABI:

- Guest `sceSifBindRpc` validates its client descriptor,
  server ID and mode and stages an actual IOP-to-EE 36-byte
  RPC BIND packet. A synchronous guest caller blocks.
  The client is **not bound** until a matching EE Rend arrives
  with nonzero server data and a validated EE buffer.
- Guest `sceSifCallRpc` checks the bound client,
  stacked MIPS O32 arguments, source/receive memory spans,
  method, mode and payload length. It snapshots real IOP
  bytes and stages the original 56-byte CALL packet with
  the client/server identity and EE destination.
- The outbound packets use the existing queued SIF0
  transport: **only an armed EE DMAC channel 5** transfers
  the command and optional payload. There is no fake EE
  server invocation or completion.
- A real incoming 48-byte SIF1 Rend must match the
  pending client pointer, packet/transaction ID and
  BIND/CALL command. Only then are the guest client's
  bound/busy fields updated, and any blocked guest
  thread awakened. A mismatched/stale Rend rejects the
  entire staged SIF1 descriptor list without DMA side
  effects, including data transfers before the packet.
- Asynchronous callback pointers are deliberately
  **rejected**, not silently ignored: scheduling genuine
  IOP R3000 callback execution needs a separate audited
  callback trampoline. Callers using the supported
  blocking or callback-free NOWAIT form retain genuine
  pending/completion semantics.
- The guest-R3000 regression executes actual
  sceSifInitRpc, sceSifBindRpc and sceSifCallRpc import
  stubs, validates SIF0 packet/data emission, a
  blocked caller, forged reply rejection, matching
  EE-to-IOP reply/data DMA and replay rejection.

The native HLE registration count is now **179**.
SIFCMD #15/#16 are defined in reference `sif.cpp`, so
the **131/172** explicit reference `libs.cpp` audit
is unchanged. Only the next matched real Windows boot
can prove whether CRI_ADXI actually links/starts or
uses an unimplemented callback/EE server path. Do not
infer sound output or GS rendering from CI.

## 2026-11-20 — CRI_ADXI SYSCLIB formatted output imports

The next matched Windows archive `ps2-game-hle(6).zip` includes all
five diagnostic files and three all-black GS captures. Its final
`SLUS_214.42` replay matches the expected USA ELF SHA-256 and
disc folder. **14,805 EE dispatches** (no change from the previous
Windows boot), **0 new EE function modules** in expansion, and
**3,526 translation-cache entries**. Four actual LOADFILEs
continue to link and start guest module threads:
`SIO2MAN.IRX` (1/1), `MCMAN.IRX` (2/3),
`MCSERV.IRX` (3/4), and `LIBSD.IRX` (4/5).
Their startup is not proof of completed module initialization.
The genuine `CRI_ADXI.IRX` method-0 LOADFILE attempt (seq 17)
now fails linking on **`sysclib#19 sprintf`**, rather than
the previous `sifcmd#16`. The EE ends at the same
`KExit` syscall, EPC `0x001748C4`, with no nonblack GS pixels.

The reference `ps2-web-recomp/runtime/src/iop/libs.cpp`
implements both `sysclib#19 sprintf` and
`sysclib#42 vsprintf` using a 32-bit guest formatting helper.
The native kernel HLE now registers and executes both in one
coherent formatting batch. This is **real guest output**, not a
return-zero success stub:

- `#19` reads format from IOP RAM, then O32 variable arguments
  beginning at `a2/a3`, with subsequent words at `sp+16`.
  `#42` reads the caller's guest `va_list` through validated
  32-bit IOP RAM accesses.
- Supported C format conversions: signed and unsigned decimal,
  hex/octal, pointer, byte character, bounded guest strings,
  double-precision floating point, escaped percent, widths,
  precision and common formatting flags; `ll` arguments and
  double words use the guest's 64-bit argument alignment.
  PS2SDK's IOP `long` remains 32 bits, regardless of host
  machine ABI.
- Format and each requested argument have hard upper limits.
  Guest format/string/pointer reads and the complete destination
  span are verified **before any output write**. Invalid
  pointers, unsupported conversion patterns, oversized widths
  and dangerous `%n` are refused without changing guest
  destination RAM or faking a formatted string.
- Regression tests execute actual guest-import calls for
  `sprintf` and `vsprintf`, compare NUL-terminated guest
  output and return byte counts, cover 64-bit integer/double
  guest arguments, and verify transactional refusal.

Registration is now **181 native HLE ordinals**, including
**148** in the kernel bridge. Since the added services are
both explicitly defined in the reference `libs.cpp`,
reference matches increase from 131 to **133/172**; the
`sysclib` family is **29/31**. No other device-driver or
EE rendering behavior is claimed.

**Next matched Windows boot is required** before declaring
`CRI_ADXI.IRX` successfully linked or started, since CI
only validates synthetic guest import and ABI fixtures.

## 2026-11-20 — CRI_ADXI VBLANK wait batch (archive 7)

The matched Windows final replay in `ps2-game-hle(7).zip` has 14,805 EE
dispatches, 3,526 translation-cache entries, zero new IR functions,
17 committed LOADFILE requests and four actually started IRX modules.
`CRI_ADXI.IRX` still fails its *link*, but its first unresolved import
has changed from `sysclib#19` to `vblank#5`. The terminal guest
`KExit` EPC remains `0x001748C4`; all three GS PPMs contain zero
nonblack pixels. This is evidence that the previously added SYSCLIB
formatter cleared a link dependency, **not** proof of gameplay,
audio initialization or successful CRI startup.

The reference `ps2-web-recomp/runtime/src/iop/libs.cpp` and PS2SDK
`iop/kernel/include/vblank.h` agree on VBLANK ordinals `#4..#7`.
They now install executable R3000 import stubs with real thread
blocking: `#4/#6` wait for the next VBlank **start**, and `#5/#7`
wait for the next VBlank **end**. A separately delivered matching
edge wakes the original guest thread and returns zero. Opposite edges
do not wake it; no host timeout, synthetic interrupt or fake GPU
present event is used. Wait state is cleared on IOP reboot.

The EE runner forwards **start** only when its scanout scheduler has
actually delivered a field-start event. The current scanout clock
does **not** expose a validated end-edge event, so `#5/#7` will
remain blocked if invoked until that hardware-model event is
implemented. The reference's callback registration `#8/#9` remains
unregistered: no guest callback or interrupt-context completion is
invented. The reference-compatibility EE clock is not hardware cycle
calibration.

Regression tests use actual guest R3000 calls for every wait ordinal,
assert no wake without a matching edge, test start/end disjointness,
and verify the guest resumes and reaches its own breakpoint after
the matching edge. Link eligibility has advanced in synthetic tests;
only a new matched Windows archive can establish actual CRI link
and startup outcomes.

## 2026-11-20 — archive 8: ARKD_DVD CDVDMAN + LIBSD SPU2 SSBUS

Archive `ps2-game-hle(8).zip` contains a matched USA retail final replay,
five JSON/log diagnostic artifacts and three black PPM captures. SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`
and the mounted disc root both match the baseline.
Final replay: **11,295 dispatches**, 3,529 cache entries, **three new
translated functions** discovered during auto-expansion.
Unlike archive 7, `CRI_ADXI.IRX` really linked and started:
committed LOADFILE request 17 returned module 5/thread 7, zero link error.
Request 18 attempted `ARKD_DVD.IRX` but failed to link with
`cdvdman#5` (`sceCdStandby`); no module or thread was created.
Separately the genuine `LIBSD.IRX` thread 5 faulted on a 32-bit
write to `0xBF801404` (PC `0x00059774`), a SPU2 SSBUS
configuration register. This is not an audio sample or DMA transfer.
The terminal EE condition also changed to exception code 8 at
EPC `0x00175064`; it is not the earlier KExit. GS accepted 37
batches, but all three PPMs contain zero nonblack pixels.

Based on `ps2-web-recomp/runtime/src/iop/cdvd.cpp` and PS2SDK's
`cdvdman.h`, `cdvdman#5 sceCdStandby` is an idempotent
standby request for the already mounted extracted-disc backend.
`cdvdman#11 sceCdSync` returns the idle queue state; it does
not complete physical N-commands, DMA, callbacks or a spindle
transition. These two handlers have executable guest import stubs;
unknown CDVDMAN commands remain unregistered.

Following PS2SDK's `ssbusc.c` and PCSX2's IOP hardware map,
IOP MMIO now retains 32-bit SPU2 SSBUS base-address and
delay registers at `0x1F801404`, `0x1F80140C`,
`0x1F801014` and `0x1F801414`. They reset with the
IOP and reject unsupported widths. They do not expose SPU2
audio RAM, generate sound, launch DMA or assert interrupts.
Regression tests exercise direct guest-KSEG1 CPU access,
readback, rejection of unimplemented MMIO, reset behavior and
the newly linked `sceCdStandby` R3000 import.

**Important:** the LIBSD guest MMIO repair and ARKD_DVD module
link eligibility are synthetic/structural claims only until the next
Windows replay. The lower EE dispatch count is not evidence of
regression by itself; the new path terminates at an entirely
different EE EPC and new IOP guest fault.


## 2026-11-20 — Windows archive 9: CDVDMAN asynchronous read batch

The matched `ps2-game-hle(9).zip` has the expected USA ELF hash
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`,
the same extracted-disc root, 15,057 EE dispatches, 3,529 cached IR
functions, and five successfully linked/started guest IOP modules.
LOADFILE request 18 for `ARKD_DVD.IRX` still failed linking, but
its first missing import changed from `cdvdman#5` to `cdvdman#6`.
No ARKD module ID or thread was created; this is a link dependency,
not evidence that a guest read was executed. The EE stopped at
syscall-exception EPC `0x001748C4`. All three PPMs are fully black.

New `sceCdRead` HLE uses the validated folder-sector source and
reference `ps2-web-recomp/runtime/src/iop/cdvd.cpp` command state.
Only actual mounted 2048-byte sectors with a fully validated IOP
guest-RAM output region are accepted. Actual disc data is staged
at request time, then committed after an explicit virtual IOP
instruction-clock event. No host-time polling, physical CDVD IRQ,
DMA completion, SIF reply or unimplemented guest callback is invented.
The compatibility delay is not calibrated to reference physical
400us + 250us-per-sector disc timing.

`sceCdSync` polling returns the genuine pending state; a nonpolling
wait blocks the caller until the staged transfer is committed.
`sceCdStatus` reports busy during the pending interval. The kernel
advances explicitly scheduled HLE events while waiting and clears
them on reboot. Synthetic guest R3000 tests cover import linking,
the original JAL import call, exact returned mounted-sector bytes,
polling/busy/idle states, invalid-source rejection and zero
fabricated interrupt.

ARKD_DVD additionally imports CDVDMAN `#15/#37/#38/#75`,
`vblank#8` and LIBSD functions. Their runtime behavior remains
unverified; static import inventory alone does not establish it.


## 2026-11-20 — Windows archive 10: complete observed CDVDMAN import set

The matched `ps2-game-hle(10).zip` replay has the expected USA
`SLUS_214.42` SHA-256 and extracted disc root. Its final replay still
reports 15,057 EE dispatches, 3,529 translation-cache entries, five
linked/started IOP modules and 18 committed RPC/LOADFILE operations.
The first unresolved ARKD_DVD import moved from `cdvdman#6` to
`cdvdman#15 sceCdStop`; ARKD_DVD itself remains unlinked, with
no module ID or guest entry thread. The EE still ends with a syscall
exception at EPC `0x001748C4`. All three PPM snapshots are black.

The full ARKD_DVD static CDVDMAN import inventory contains #4, #5,
#6, #8, #10, #12, #13, #15, #28, #37, #38, #75. The only missing
CDVDMAN exports before this change were **#15 sceCdStop,
#37 sceCdCallback, #38 sceCdPause, #75 sceCdMmode**.
`MCMAN.IRX` imports only #24, which is already implemented. These
are the complete observed CDVDMAN import gaps in the archive, not
a statement about unscanned modules in future boot paths.

For extracted, already mounted media, Stop and Pause acknowledge
idempotent spindle requests without mutating the actual queued
file-backed transfer or inventing completion. Mmode stores only
supported auto/DVD selections; a request to switch to absent CD
media is rejected. These calls do not interact with physical CDVD
DMA or pretend that another disc was inserted.

Unlike a fake callback registration stub, sceCdCallback records and
returns the previous validated guest handler pointer, retains its
registering-module GP and dispatches **actual guest R3000 code**
with the CDVD read completion reason after a staged file-backed
read finishes. A one-shot kernel callback thread is entered through
the registered guest function and released when it returns to the
dedicated R3000 breakpoint trampoline. This is a guest-thread HLE
approximation to interrupt-context delivery, **not** proof of
hardware-accurate interrupt timing or physical disc operation.
The guest completion handler is never invoked merely by linking
or by calling Stop/Pause/Mmode.

Regression coverage exercises all 14 executable CDVDMAN import
stubs, actual IRX guest callback registration and replacement,
guest instruction execution and GP preservation on genuine sector
completion, protection against early callbacks, and no fabricated
physical IRQ or N-command completion. Unsupported seek #7 and
other non-catalog hardware commands remain unregistered.

**Next Windows milestone:** Verify whether ARKD_DVD links and starts.
The static inventory also names `vblank#8` and LIBSD dependencies,
which have independent behavior and may become subsequent blockers.
No game-boot or visible-frame claim is justified by import parity
and synthetic CI alone.

## 2026-11-20 — matched Windows 02:26:45 archive: SIFCMD and VBLANK import parity

The matched final replay confirms that the previous four CDVDMAN
import blockers (#15/#37/#38/#75) were resolved. It still has five
started IRX modules, 18 committed RPC operations, 15,057 EE
dispatches and 3,529 translation-cache entries. The next unresolved
ARKD_DVD import is **`sifcmd#12 sceSifSendCmd`**; no ARKD module
or thread was started. The terminal EE exception is unchanged at
EPC `0x001748C4`. PPM captures remain fully black.

The full seven-module static catalog contains exactly four previously
unregistered imports in these two libraries: `sifcmd#12` used by
ARKD_DVD, `sifcmd#28 isceSifSendCmd` (intr variant) used by PADMAN,
and `vblank#8 RegisterVblankHandler`/`#9 ReleaseVblankHandler`
(PADMAN uses both; ARKD uses #8). Other observed imports in those
two libraries already had implementations. Static dependencies
are not proof of runtime handler execution.

The native SIFCMD bridge now stages **actual R3000 guest command
packet bytes**, with the two stack-passed extra-data parameters,
and enqueues them into the existing IOP-to-EE SIF0 DMA queue.
Enqueueing does not synthesize an EE reply or DMAC interrupt; an
actual armed EE SIF0 receiver must accept the packet. Bounded
packet/payload lengths and guest IO/EE memory spans are validated.
The intr-context ordinal uses the same transport and does not
fabricate an IOP interrupt.

The VBLANK bridge now retains actual guest handler pointers and
registration GP per start/end edge, supports removal by
(edge, function), and calls the **real R3000 guest callback**
through a one-shot return trampoline only when the scanout
scheduler delivers the matching edge. No registration or wait
manufactures an edge. The existing EE scanout producer supplies
start edges; end-edge delivery is still unsupported until a
real producer exists. Registration is cleared on IOP reboot.

Regression tests verify the original guest R3000 import stubs,
complete SIFCMD packet plus extra payload traversing an armed
EE SIF0 receiver, atomic failure for invalid guest spans,
VBLANK edge matching, actual callback argument execution,
registration removal and reboot clearing.

**Next Windows replay:** confirm that ARKD_DVD now links and
starts; otherwise diagnose its newly exposed first unresolved
import. No playable boot, sound or rendered image is implied.

## 2026-11-20 02:55 — ARKD guest RPC bind (matched Windows evidence)

Matched archive: `ps2-game-hle(20261120-025514).zip`, USA ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The final committed replay starts all six modules: SIO2MAN, MCMAN,
MCSERV, LIBSD, CRI_ADXI and ARKD_DVD (module 6/thread 8), with no
unresolved IRX imports. The five earlier modules remain started.
Successful linkage and thread creation do not establish module initialization.

The next genuine EE SIF1 packet is a 64-byte BIND, CID `0x80000009`,
server `0x500`, client `0x005A9330`, IOP destination `0x0001E000`,
rejected as `unknown_server`. The final replay stops after 11,429
EE dispatches, with 3,530 translation-cache hash entries and
syscall exception EPC `0x00175064`. All three GS PPMs are black.
The earlier 15,057 dispatches are a different stop path, not
evidence of a loss of working IOP modules.

The primary `ps2-web-recomp/SDBZ` reference, `runtime/src/iop/sif.cpp`,
defers an EE bind until a guest IOP RPC server actually registers.
Its Arika RPC trace explicitly recognizes server IDs `0x500..0x503`.
The native implementation now stages only those specifically identified
guest-owned server IDs when not yet registered; unrelated unknown
servers and unsupported CALL methods still fail closed.
The bounded pending bind is released only after real guest
`sceSifRegisterRpc` and `sceSifSetRpcQueue` have made a server ready.
No SIF0 Rend, client binding, guest callback, or completion interrupt
is synthesized before that event. Reset clears pending bind state.

The reference has an IOP-clock-based bind timeout. This native bridge
does not yet have a calibrated shared deadline for these requests;
they remain bounded by queue capacity but can wait indefinitely.
The next matched Windows archive must establish whether ARKD_DVD's
guest thread really registers server `0x500` and can service its
subsequent RPC operations; passing synthetic CI is not boot validation.

## 2026-11-20 03:20 — Guest IOP progress while EE waits for ARKD bind

Matched archive: `ps2-game-hle(20261120-032006).zip`. Same USA
`SLUS_214.42` SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`,
same extracted disc and final committed replay (`replay=0`); no expansion
retry or new function generation. All six LOADFILE modules still link
and start; `ARKD_DVD.IRX` is module 6, initial guest thread 8.

PR #145's deferred `0x500` BIND now commits as SIFRPC sequence **19**
(client `0x005A9330`). Its registered guest server has not yet been
observed. The runtime executes one 256-instruction IOP scheduler turn
after the SIF1 transaction, but this is not proof of server registration;
the diagnostic `thread=0` is an unpopulated result field on
`budget_exhausted`, not evidence that no IOP thread executed.

The EE next traps at a real **WaitSema** (`v1=0x44`, A0 semaphore 8),
not `_SetAlarm` (`v0=0x18` is a stale return register). The runner
refuses the zero-count wait because there are no ready EE peers and its
existing IOP-completion advancement is enabled only for ROM0 boot.
Final replay: **11,434 EE dispatches** (+5), 19 committed RPCs,
terminal EPC `0x00174CE4`, Cause `0x20`. The three GS PPMs are
entirely zero. No new import, unsupported instruction or MMIO fault
is demonstrated by this archive. The translation cache does not report
a new function in the final replay.

**Change:** Extend the existing no-ready-EE-peer WaitSema rendezvous
to the HLE-first path only while a real deferred EE RPC BIND exists
**and** `c_kernel::has_guest_work()` finds ready/timed IOP work.
The IOP executes a bounded guest R3000 quantum; the RPC bridge may
pump only an already-queued SIF0 reply, and the EE still requires the
actual DMAC5 interrupt and original guest iSignalSema to finish
WaitSema. No fabricated server, reply, interrupt, wakeup or virtual
clock completion is permitted. Preserve the ROM0-only path unchanged.

The reference `ps2-web-recomp/SDBZ` drives its IOP kernel from elapsed
EE cycles (see `runtime/src/iop/kernel.cpp::run`), processes guest
thread scheduling and commands while EE services are blocked, and
defers SIFRPC BIND until the guest registers a matching server
(`runtime/src/iop/sif.cpp`). The native runner is still using a
bounded uncalibrated IOP guest-instruction compatibility clock; this
batch is a targeted EE/IOP rendezvous, **not** calibrated full
EE/IOP parallel scheduling.

Regression: no IOP guest thread -> no wait advancement; dormant guest
thread -> no advancement; genuine started R3000 thread -> bounded
execution but no premature Rend; only registration moves the pending
bind into a SIF0 reply, and only an armed SIF0 can deliver it.
Existing standalone EE WaitSema negative tests continue to require
fail-closed when no work or peer exists.

**Next matched Windows validation:** confirm whether the HLE WaitSema
guest turns reach actual ARKD guest registration for server `0x500`,
whether a real DMAC5 callback signals the semaphore, and identify
the next supported or faulting guest instruction. A bounded wait
exhaustion remains a reported blocker, not synthetic success.

## 2026-11-20 03:36:57 — IOP R3000 RAM mirror fault after EE WaitSema rendezvous

Matched archive: `ps2-game-hle(20261120-033657).zip`. Only the
committed `replay=0` is present; `boot.log`, `expansion.log`,
`first-boot.json`, `iop-disc-catalog.json`, `translation-cache.json`
and all three GS PPMs belong to that replay. The ELF SHA-256 is
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

All six committed IRX loads again link/start; the `ARKD_DVD.IRX`
LOADFILE is sequence 18 (module 6, initial thread 8), followed by
committed 64-byte SIFRPC BIND sequence 19 (server `0x500`, client
`0x005A9330`), pending and without any observed registration or SIF0
reply. The EE stops at `WaitSema(8)`, EPC `0x00174CE4`, Cause `0x20`,
11,434 dispatches. The HLE-first WaitSema rendezvous from draft
PR #146 executes one guest IOP turn (256 R3000 instructions) and
then stops on `e_kernel_stop::memory_fault` (`reason=4`), thread 1,
PC `0x00202080`, opcode `0x00000000`, after another 38 guest
instructions. This is an instruction-fetch failure outside the
native CPU's previous strict 2 MiB RAM window, not an observed
unresolved IRX import or proven failure in ARKD's initial thread.
All three PPM captures have zero nonblack pixels.

### Reference parity and implementation

The primary `ps2-web-recomp/SDBZ` reference uses `ram_ptr(a)` with
`phys(a) & (kRamSize - 1)` and fetches IOP instructions as RAM for
physical addresses below `0x00800000`. PCSX2's IOP memory LUT
similarly maps 2 MiB RAM across the first 8 MiB of physical space
(its first 0x80 64-KiB pages index RAM with `i & 0x1f`).
Consequently the attempted PC `0x00202080` aliases backing offset
`0x00002080`, and native `c_cpu::read`/`write` must preserve that
address translation for aligned instruction fetches and data accesses.

Add a strictly bounded 8 MiB IOP RAM mirror window to `c_cpu`,
retaining the 2 MiB backing store, original guest virtual addresses,
the KSEG0/KSEG1 restrictions, alignment checks, ROM0 and MMIO
routing, and rejection at or above physical `0x00800000` when
unmapped. Leave the existing DMA buffer and kernel allocation
ownership checks conservative; this batch does not license
arbitrary DMA or heap addresses beyond actual backing RAM.

Test executed R3000 code at `0x00202080` against backing RAM
`0x00002080`; check the other mirrors and KSEG aliases, and reject
unmapped addresses, unaligned writes and KSEG2-only virtual
addresses. Preserve all existing hardware/ROM0 tests.
Add the actual fault address/access width and guest RA/GP/SP to
the EE WaitSema IOP-stop diagnostic to identify the next genuine
blocker without assuming the RAM mirror will complete RPC binding.

**Next Windows replay must confirm** whether the guest progresses
through the previous PC, whether server `0x500` registers and replies
via an armed SIF0 receiver, whether EE `WaitSema(8)` wakes through
the original DMA5 callback, and what exact IOP/EE fault follows.
Static module startup and CI tests alone do not establish gameplay.

## 2026-11-20 03:47:27 — IRX guest execution exhausts bounded wait

Matched archive: `ps2-game-hle(20261120-034727).zip` has exactly one
final `replay=0`, zero newly discovered EE functions, and consistent
`boot.log`, `expansion.log`, `first-boot.json`, `iop-disc-catalog.json`,
`translation-cache.json`, and three GS PPM captures. USA game ELF SHA-256
remains `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

The IOP RAM mirror regression has reached past the earlier attempted
fetch at `0x00202080`, without the previous memory fault. Six real
IRX loads link/start; committed SIFRPC sequence 19 queues one BIND
for ARKD server `0x500` (client `0x005A9330`). Guest registration and
an SIF0 reply have **not** been observed.

EE still stops at original `WaitSema(8)`, EPC `0x00174CE4`,
after 11,434 dispatches. The IOP advances the entire configured
2,048-turn limit of 256 guest instructions per turn (524,288 guest
instruction slots during that wait) without a fatal guest fault, yet
`binds=1 replies=0` when exhausted, and the last IOP CPU PC is
`0x0020B920`. All three GS PPMs remain entirely black.

**Do not infer successful IOP initialization from an uninterrupted
instruction count.** The final PC aliases backing IOP RAM offset
`0x0000B920` and the difference from 2,048 × 256 × 4 bytes is exactly
`0x0000B920`. This is consistent with — but does not prove — a
linear NOP walk through mirrored uninitialized RAM. The current
kernel completion result reports `thread=0` on `budget_exhausted`
even though a real thread was chosen. It does not expose whether a
high-priority guest thread starved ARKD's initial thread 8.

**Diagnostic-only changes in the existing draft PR #145:** report
the last actually scheduled IOP thread on a successful bounded turn;
expose immutable `c_kernel::threads()` snapshots; capture per-thread
status/priority/wait type/wake, entry and current PC, guest opcode
in mapped RAM, and whether each PC falls in a linked IRX image.
Log only before the first wait turn and after turns 1, 64, 512 and
the configured bound; additionally capture snapshots on real IOP
faults. Test scheduler reporting, snapshot immutability, and cleanup
after reboot.

The next Windows archive should identify actual scheduled thread
IDs and whether any guest code enters ARKD module 6. Continue to
require real `sceSifRegisterRpc`, armed SIF0 transfer, and original
DMAC5/iSignalSema completion. Do not fabricate a server, inject a
callback, alter EE semaphore count, assume unlimited budget, or
blindly mark the bind successful.

## 2026-11-20 03:57:15 — IRX module-start thread priority and return lifecycle

Matched archive `ps2-game-hle(20261120-035715).zip`: one final replay
(`replay=0`), six linked/started IRX loads, 19 committed RPCs, one
pending server `0x500` bind, 11,434 EE dispatches, and a blocked
`WaitSema(8)` at EPC `0x00174CE4`. The 2,048 IOP wait turns execute
524,288 guest R3000 instruction slots without registration or SIF0
response. All three GS PPM captures remain completely black.

The bounded snapshots prove that several IOP module-start threads are
running at priority **1** after leaving their linked IRX images and
advancing through zero-filled mirrored RAM; their retained `ra=0`
allows the original `JR $ra` to return into address zero and run NOPs.
At turn 2,048 SIO2MAN startup thread 1 has PC `0x0020FA38`, and
ARKD_DVD startup thread 8 has PC `0x0020B920`, both mapped as
`image_module=0` with zero opcodes. ARKD's guest-created worker
threads **9–12** are ready at priority **50** but remain at their
unchanged entrypoints inside module 6; module-start priority 1
starves them. A separate worker thread 13 has priority 39 and also
remains at its entry. These are actual guest-created threads, not
invented RPC services. The log supports a scheduler/lifecycle
starvation diagnosis; it does **not** establish that the ARKD server
would have registered after worker execution.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/kernel.cpp`
uses `kDefaultModulePriority = 8` for `start_module`, calls the guest
`_start(argc, argv)` with `ra=g_exit_stub`, and only when the guest
returns does its exit callback mark that startup thread dormant and
publish the actual module result. Its general `start_thread` also
installs this exit trampoline. Our HLE-first runner instead used
priority 1 and seeded module `ra=0`; the kernel only implemented
special return trampolines for alarms and one-shot callbacks.

**Fix in the existing draft PR #145:** use default priority 8 for
LOADFILE-created startup threads; keep an explicitly supplied
priority unchanged. Tag the module's real initial guest thread and
install a reserved MIPS BREAK return trampoline at `0x00013FD0`
(next to existing alarm/callback return stubs), setting the guest
`ra` whenever that module thread is started, including its
`StartThreadArgs` context reset. Only an actual guest `JR $ra`
followed by execution of the trampoline retires that module-start
thread as dormant while preserving its guest `v0` result.
This does not synthesize a successful module result, bind a server,
or signal an EE semaphore; guest background worker creation and
priority rules continue to come from executed IOP instructions.

Regressions cover a real R3000 `JR $ra` plus delay-slot result,
no further scheduling after a module returns, matching default 8
priority, and preservation of explicit caller-chosen priority.

The reference kernel also seeds `ra=g_exit_stub` for **ordinary**
`StartThread`/`StartThreadArgs` guest threads. ARKD worker threads 9–12
had `ra=0` in the same snapshot and could otherwise repeat the bad
fall-through upon returning. This batch therefore reserves a separate
`0x00013FC0` BREAK trampoline for ordinary guest threads, retains
the preexisting alarm and one-shot callback return trampolines,
and retires a generic worker only after its actual guest MIPS return.
Tests exercise that separate worker path and verify no spurious module
completion or fabricated signal.

**Limitations and next Windows replay:** The present LOADFILE bridge
publishes the module ID before observing the entrypoint's return;
full load-module completion/result timing parity remains a separate
issue. Observe genuine guest startup returns and worker execution,
then `sceSifRegisterRpc` for server `0x500`, any real SIF0 reply,
and EE `WaitSema(8)` DMA5 callback wakeup. Do not claim these steps
from CI alone.

## Windows clean Clang boot — missing runner stack reserve (2026-11-20)

The latest user-provided diagnostic set consists of `boot.log`,
`expansion.log`, `iop-disc-catalog.json`, and
`translation-cache.json` from a clean Windows LLVM `clang++` build.
It is *not* a matched final traced boot: the expansion terminated
on its first non-traced `replay=0`, before the wrapper could execute
its separate final traced replay. Both logs contain the same six
lines, ending in `exit=4294967295` (`0xFFFFFFFF`, -1 signed),
`elapsed=1.34s`, no reached EE dispatch count, and no observed
IOP runtime events. No `first-boot.json` was supplied. The catalog
correctly labels its seven IRX candidates and 279 imports as
`static-only`, with zero runtime load events. The validated
translation cache records only the entry-function IR
`sub_0x100008_0x100008.ll`; no EE expansion was possible after the
runner process quit.

Code inspection identified a concrete build configuration mismatch:
`src/tools/ee_game_runner_main.cpp` holds
`s_kernel_thread threads[256]` on the main function's stack;
each thread stores a full `s_ee_vu0_state` with two 4 KiB
micro/data memories (over 2 MiB for VU0 snapshots alone). However
`CMakeLists.txt` reserved an 8 MiB stack with `if(MSVC)`, which
misses the LLVM `clang++` *GNU-like* driver used for the user's
fresh Ninja build. Windows' default PE stack reserve is much too
small for that frame. This is a proven **configuration defect**,
not yet a proven complete explanation for the numeric `-1` exit.

PR #145 now applies the same 8 MiB stack reserve for all native
Windows compiler drivers through `LINKER:/STACK:8388608`.
The Windows native WebGPU CI job inspects the *linked Clang PE header*
using `dumpbin /headers` and requires an `800000` stack reserve;
it also executes `ee_game_runner.exe --help` as a startup smoke
check. Both MSVC and fresh Clang builds are retained. No EE, IOP,
RPC, guest timing, or shader behavior was altered.

**Next validation:** after new CI passes, re-run the existing
`run_ps2_game.bat` from the current PR branch and inspect a
matched final traced log bundle, including `first-boot.json`.
Do not attribute ARKD registration or a genuine DMA5/EE semaphore
wake to this host-only fix before observed runtime evidence.

## 2026-11-20 04:56:35 — original LIBSD SPU2 register write

Matched user Windows ZIP `ps2-game-hle(20261120-045635).zip` includes
`boot.log`, `expansion.log`, `first-boot.json`, three GS PPM images,
`iop-disc-catalog.json`, and `translation-cache.json` from the same run.
Fresh Windows LLVM native WebGPU/raylib startup now succeeds. Four
expansion passes added 128, 128, 128, and 35 IR functions, leaving
420 recorded translated IR files. The final trace reached 11,434 EE
dispatches / 16,026,712 reached guest instructions. The EE is still
waiting on semaphore 8 at EPC `0x00174CE4`; the deferred ARKD bind
remains server `0x500`, client `0x005A9330`, with no guest RPC
server registration or real reply.

**Positive lifecycle evidence:** all six actual IRX guest module
`_start` threads now return through the new `0x00013FD0`
trampoline: modules 1–6 return guest `v0` 0,2,2,0,2,0 respectively.
The high-priority module-start starvation from the prior 03:57:15
run is resolved. ARKD module 6 creates four actual worker threads
9–12 at priority 50. A LIBSD-related guest thread 13, priority 39,
runs far enough to issue a real R3000 halfword store at PC
`0x0005C8E8` (`SH`, opcode `0xA4450000`) with virtual address
`0xBF900760`. Its IOP physical address is `0x1F900760`.
The kernel stops with `e_kernel_stop::memory_fault`, address
`0xBF900760`, width 2, write 1. This is an unimplemented SPU2
**master-volume register**, not an 8 MiB IOP RAM alias, opcode
decoder failure, successful audio transfer, or EE translation gap.
The IOP scheduler cannot deliver the pending ARKD bind while this
guest memory fault remains.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp`
routes IOP physical `0x1F900000..0x1F9007FF` to its
`spu2::read/write`. `runtime/src/iop/spu2.cpp` maps each core
through its 0x400-byte window, maps master volume/control through
offsets `0x760` and `0x788`, uses 16-bit little-endian reads
and writes, and exposes the `0x7C0` SPDIF register bank. Core
`0x19A` ATTR, `0x19C` IRQA, `0x1A8` TSA, `0x1AC`
data port and `0x344` STATX require their own state/semantics.

**New PR #145 batch:** add an isolated `iop::c_spu2` register
model routed through `iop::c_hw`; map and reset the original
master/core/SPDIF banks; preserve guest-programmed 16-/32-bit
register values, IRQA/TSA, statx settle and 2 MiB sound-RAM
data-port state, and only assert IRQ9 for a genuine guest-selected
SPU port/IRQA collision. Reject unsupported widths or dynamic
voice envelope/NAX reads once playback is requested. This is an
intermediate **register/port** milestone, not a sound engine.
No voice mixer, sampled audio tick, DMA4/7 completion, generic
arbitrary MMIO ready bit, or ARKD RPC response is synthesized.
A R3000 `SH` test uses the exact original faulting opcode and
address; additional regressions check master readback, core
isolation, STATX settle, sound-RAM port/IRQ semantics, invalid
accesses and reset.

**Next evidence needed:** an authentic Windows replay after
pulling the unchanged PR branch. Revisit the IOP scheduler,
record whether the original SPU2 store now executes, and identify
the next guest SPU2 status/DMA operation or real
`sceSifRegisterRpc(0x500)` call. No game boot success is implied.

## 2026-11-20 05:09:58 — ARKD guest RPC server registered; thsemap #8 error

Matched Windows replay `ps2-game-hle(20261120-050958).zip` contains
`boot.log`, `expansion.log`, `first-boot.json`, the two native
WebGPU/GS diagnostic snapshots plus upload PPM,
`iop-disc-catalog.json` and `translation-cache.json`.
The SPU2 register failure from the preceding 04:56:35 batch has
cleared: the final replay reaches 11,773 EE dispatches, 16,030,642
guest instruction attempts, 421 recorded LLVM IR files, 29 committed
IOP SIF RPC requests and all six original module-start returns.
All three PPM captures remain entirely black.

**Positive milestone**: the genuine ARKD guest worker registers
SIF RPC server `0x00000500`, releasing the one previously deferred
bind for EE client `0x005A9330`. The EE then binds the other
ARKD servers `0x501`, `0x502`, `0x503`, issues method `0x12`,
method `0x11`, and requests method `0x01` on `0x500`.
This is real IOP guest registration and attempted RPC dispatch,
not an invented ready flag or host-owned success response.
No end-to-end method return, DMA5 reply, or game rendering is yet
verified; the final EE exception at `0x80000180` is a stop caused
by the IOP service failure.

**New blocker**: when the guest RPC queue calls method `1`, IOP
thread 9 stops after 81 executed instructions at `0x000147A8`,
opcode `0x0000000C`, `e_kernel_stop::unhandled_syscall = 2`.
The exact HLE syscall stub offset
`0x147A8 - 0x14000 - 8 = 61 * 0x20` identifies export table
index 61, `thsemap` ordinal `8` / `WaitSema`. This is a
*handled, documented IOP library function*; the bridge incorrectly
returned false for an unknown guest semaphore ID, converting a
guest API error into a terminal host syscall failure. Without the
guest `a0` value in this batch, the reason the requested semaphore
ID is missing remains unverified.

**Primary reference**: `ps2-web-recomp/SDBZ/runtime/src/iop/libs.cpp`
defines unknown semaphore `-408`, unknown event flag `-409`,
unknown mailbox `-410`, unknown variable pool `-411`, and unknown
fixed pool `-412`. These errors are genuine negative guest return
values, *not* successful waits or fabricated kernel objects.
The original reference `WaitSema` explicitly returns `-408`
when its ID does not exist.

**PR #145 implementation**: the five corresponding guest HLE
kernel families return reference errors for *known imported
operations* targeting missing IDs, while unsupported ordinals and
unexpected internal failures remain fail-closed. Invalid
`thsemap` #8 requests emit a once-per-unknown-ID diagnostic
containing requested ID, guest thread, PC, RA and currently
registered semaphore counts/IDs. The unknown-ID diagnostic set
is cleared on IOP reboot. Regression tests execute real R3000
HLE import stubs after deleting a known semaphore/event and
check all five negative error code families; unknown IDs do
not allocate, wake, signal or fake a completion.

**Next matched replay**: inspect `IOP HLE WaitSema unknown id`
and the actual guest response after `-408`; determine whether
the RPC method handles it or needs further authentic producer/
allocator investigation. Keep SIF0/DMA5 and guest RPC completion
evidence separate from successful server registration.

## 2026-11-20 05:21:52 — ARKD method 1 survives unknown WaitSema; IOP scheduling gap

Matched replay `ps2-game-hle(20261120-052152).zip` contains boot and expansion
logs, first-boot report, disc catalog, translation cache and all three GS
snapshots. The native runner reuses 421 cached LLVM functions, reaches the
100,000 EE dispatch ceiling (16,912,906 EE instruction attempts), and
commits the same 29 SIF RPC commands. Six original IRX start threads and
ARKD server registrations `0x500..0x503` remain established. All three
images are black. This is not a successful game boot.

The prior fatal `thsemap` #8 at IOP PC `0x000147A8` is now handled as the
reference kernel error `-408`; its requested ID is **zero**, not an
allocated semaphore. The diagnostic reports IOP thread 9, RA `0x0007D648`,
and existing semaphore IDs `2..8`. Do not invent ID zero or imply its
producer has been found. Guest methods `0x12` and `0x11` reach idle after
65 and 60 R3000 instructions respectively and have subsequent observed
EE DMA5 callbacks. Method `0x01` instead exhausts the initial 256-step
IOP guest quantum without a verified finish/reply.

From EE dispatch 11,779 onward the native trace alternates between
`0x00178DE8` and `0x001BD58C` and reaches its dispatch limit. The
runner previously invoked guest IOP slices only at selected EE SIF
HLE boundaries and trapped `WaitSema`, not while EE executed this
translated-code polling loop. The primary `ps2-web-recomp/SDBZ`
`runtime/src/iop/kernel.cpp` advances IOP work against elapsed EE
cycles. The follow-up change therefore schedules at most one bounded
256-instruction R3000 slice per 2,048 elapsed EE guest cycles while an
actual guest RPC remains pending and IOP work is runnable. This is a
narrow HLE-first scheduling step, **not** a full calibrated IOP clock.

The IOP guest call pending count is read from actual unfinished RPC
records. Errors and guest memory faults propagate as explicit stops;
no reply, DMA5 completion, semaphore or readiness is synthesized. A
regression checks that a committed guest request remains pending with
unchanged EE return memory and no DMA5 until its genuine R3000 handler
finishes; the pending count then drops to zero. CI and the next local
Windows replay must validate the runtime path. In particular, the
origin and guest handling of `WaitSema(0)` remain unresolved.

### Windows Clang CI environment regression at commit 6123aa14

The first CI run after the 05:21:52 IOP scheduling batch passed
SIF DMA acceptance and ROM0 acceptance. The Windows native WebGPU
MSVC compile and Linux native WebGPU job passed; the *separate*
Windows Clang clean-tree build failed before project code compiled:
the hosted image's LLVM/Clang 20 is rejected by Visual Studio 2026
MSVC STL 14.52.37025 (`yvals_core.h` STL1000 requires Clang >=22).
This was a compiler/STL image mismatch, not an IOP logic regression.
Pin the official LLVM 22.1.8 Windows installer in the existing
native WebGPU workflow with its published SHA-256; do not define
`_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` to suppress the STL
version guard. Final CI acceptance still requires passing jobs on
the replacement commit, not only on the earlier implementation.

## 2026-11-20 05:39:29 — guest ARKD RPC scheduler reveals LIBSD SPDIF control write

Matched Windows replay `ps2-game-hle(20261120-053929).zip` includes the
complete five-report diagnostic set and three GS PPM captures. The preceding
EE-cycle-driven IOP scheduling batch is running: it advances genuine guest
R3000 code beyond the initial 256-instruction method-`0x500:1` slice.
The replay now stops after **11,980 EE dispatches** and **16,032,692 EE
instruction attempts** with 421 cached LLVM functions, 29 committed RPC
commands, and the original six IRX module-start threads. All three GS
images are fully black (display 512x448, upload 1024x64, WebGPU 512x448).
Lower EE dispatches are due to an earlier explicit hardware stop, not a
successful boot or regression to a prior stage.

**Exact stop:** IOP thread 9, guest PC `0x0005A824`, R3000 opcode
`0xA4A20000` (`sh v0, 0(a1)`), value `8`, effective address
`0xBF9007CA`, width 2, fault reason `memory_fault=4`, pending guest
RPC calls 1. The same once-per-ID `WaitSema(0)` diagnostic occurs at
`0x000147A8` with RA `0x0007D648`; its producer remains unverified.
The EE resumes no genuine `0x500:1` reply or new DMA5 signal after this
fault.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/spu2.cpp`
`write_global()` explicitly recognizes MODE `0x7C6` and MEDIA
`0x7C8` and ignores other global writes, without implying playback,
DMA or interrupt completion. The secondary PS2SDK
`iop/sound/libsd/src/freesd.c`, `InitSpdif()`, explicitly programs
MODE=`0x900`, MEDIA=`0x200` and `U16_REGISTER(0x7CA)=8` in that
order. This matches the exact guest value and address in the replay.
The primary reference `read_global()` reads the unlatched `0x7CA` as
zero. Nearby undocumented `0x7C4` and `0x7CC` remain unsupported in
the HLE-first model.

**Batch implementation:** Add only the recognized `0x7CA` SPDIF control
halfword: accept its write without side effects; return reference zero on
read. Do not generalize this to all missing SPU2 MMIO offsets and do not
fabricate readiness, IRQ9, DMA completion, or ARKD replies. The focused
R3000 regression stages the exact opcode at the faulting PC, runs the
original SPDIF initialization register sequence, checks unchanged
MODE/MEDIA/OUT readback, rejects nearby unverified addresses and
unsupported byte width, and verifies that no IOP interrupt occurs.

**Next verification:** require the full CI suite on the final branch
HEAD and a fresh matched Windows replay. Confirm the guest passes
`0x0005A824`, then inspect genuine ARKD method `0x500:1` completion,
SIF0/DMA5 delivery and any further verified IOP dependencies.

## 2026-11-20 05:46:12 — authentic DMA4 interrupt disable during ARKD RPC

Matched Windows replay `ps2-game-hle(20261120-054612).zip` contains `boot.log`,
`expansion.log`, `first-boot.json`, `iop-disc-catalog.json`,
`translation-cache.json`, and all three GS/WebGPU PPM captures. It reaches
**18,365 EE dispatches**, **16,096,545 guest instruction attempts**, reuses
**421 cached LLVM functions**, and commits **29 RPC commands**. All six
original IRX startup threads finish and server `0x500` really registers,
releasing its deferred bind. All three images remain black; ARKD method
`0x500:1` has not returned. There are 32 observed EE DMA5 callbacks, but
none is evidence that the still-pending method `1` has replied.

**Previous blocker cleared:** the original LIBSD write at SPU2
`0xBF9007CA` no longer stops the IOP. The bounded guest RPC scheduler now
runs at least four additional 256-instruction R3000 slices. The existing
unknown `WaitSema(0)` diagnostic persists without stopping the guest;
its producer remains unresolved.

**New exact stop:** IOP thread 9, `pc=0x000140E8`, R3000 opcode
`0x0000000C`, service `v0=0x494F1207`, argument `a0=0x24`,
return address `ra=0x0005D55C`, reason `unhandled_syscall=2`,
`pending_calls=1`. The installed HLE stub at `0x14000 + 7 * 0x20 + 8`
resolves to `intrman` ordinal `7` / `DisableIntr` for IOP interrupt
line **36**, i.e. primary DMA channel **4**. The old bridge rejected
all numbers above 31 and required a registered handler.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/libs.cpp`
implements `DisableIntr` by clearing the hardware interrupt enable and
optionally writing the source line to the caller's output pointer. Its
`runtime/src/iop/hw.cpp` maps extended INTRMAN sources **32..38** to
`DICR` bits 16..22 (line 36 = bit 20) and **40..45** to `DICR2`
bits 16..21. The shared DMA IRQ is ordinary IOP `I_STAT/I_MASK` bit 3.
Line 39 and IDs above 45 are not established DMA interrupt sources.
The reference `RegisterIntrHandler`, `ReleaseIntrHandler`,
`EnableIntr`, and `DisableIntr` do not require an already registered
handler before changing a valid source's mask; release is idempotent.

**Coherent parity batch:** expose a bounded hardware `set_interrupt_mask()`
for valid primary and extended DMA sources with explicit DICR/DICR2
channel enables. Enabling a DMA source sets the shared IRQ3 mask and
primary controller master enable; disabling clears only that source's
mask, preserving pending flags and unrelated interrupt control. Update
the four INTRMAN HLE operations to use this hardware mapping and the
reference guest pointer semantics. Keep unknown service ordinals,
reserved line 39, out-of-range IDs, invalid function pointers and
unwritable output pointers fail-closed; do not generate completion,
DMA flags, IRQ delivery or semaphore wake.

Regressions execute real guest R3000 INTRMAN stubs for line 36 and
secondary line 41, verify DICR/DICR2/I_MASK state, no spontaneous IRQ,
optional guest output, registration/release, invalid-source rejection,
and reboot reset. The actual game still needs a matched Windows replay
to verify that this extended DMA4 disable advances method `0x500:1`
toward its genuine guest handler return and SIF0/DMA5 reply.

## 2026-11-20 05:57:06 — ARKD clears extended INTRMAN and reaches SPU2 DMA4 CHCR

The matched `ps2-game-hle(20261120-055706).zip` archive contains the
five diagnostic files and three P6 GS/WebGPU PPM captures. The previous
`DisableIntr(36)` syscall stop is absent, confirming that the extended
DICR interrupt-mask support advances the guest. At **41,025 EE dispatches**
(versus 18,365 previously), the replay reports **16,323,145 EE guest
instruction attempts**, **421 cached LLVM functions**, **29 committed
RPC commands**, and the same six completed original IRX startup threads.
The 512x448 GS display, 1024x64 GS upload and 512x448 WebGPU snapshots
contain only zero RGB values. ARKD `0x500:1` remains a real pending guest
RPC and there is no verified SIF0/DMA5 reply for this method.

**Exact stop:** IOP thread 9, PC `0x0005B450`, opcode `0x8E620000`
(`lw v0, 0(s3)`), guest address `0xBF8010C8` (physical
`0x1F8010C8`), width 4, memory-fault stop reason 4, RA
`0x0005AAE0`, pending guest calls 1. This register is **SPU2 core 0,
primary DMA channel 4 CHCR**. The preserved `WaitSema(0)` diagnostic
still appears; the origin of its invalid ID is unverified.

**Primary reference** `ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp`
`dma_channel()` maps all primary DMA channel register slots from
`0x1F801080` and secondary slots from `0x1F801500`. In particular,
SPU2 DMA4 uses `0x1F8010C0..0x1F8010CF` and SPU2 DMA7 uses
`0x1F801500..0x1F80150F`, each with MADR, BCR, CHCR, TADR words.
Its `start_dma()` invokes actual `spu2::dma()` with guest memory and
sound-RAM transfers before any completion is scheduled. The existing
LLVM HLE-first model does not yet supply that real transfer producer.

**Coherent batch:** Add explicit independent 32-bit DMA4/DMA7 register
banks with genuine readback, KSEG1 aliasing, reset and CHCR.STR latch
behaviour. Do **not** synthesize sound-RAM copying, clear a requested
CHCR.STR, fabricate DMA4/7 completion flags/IRQ3, or manufacture ARKD
RPC replies just because control registers are programmed. Keep
subword and unallocated MMIO addresses fail-closed. Regress the exact
original guest R3000 `lw v0, 0(s3)` at `0x0005B450` as well as both
DMA banks, separate configuration, no-fake-interrupt and cold reset.

**Next validation:** verify CI and a new matched local Windows replay
that passes the original CHCR read. If ARKD next actually requests a
SPU2 DMA transfer, implement an authentic bounded IOP↔sound-RAM DMA4/7
backend using the guest MADR/BCR/CHCR and original timing/IRQ model;
never clear a requested transfer without moving its real data.

## 2026-11-20 06:03:34 — ARKD method 1 reaches DMA4 BCR halfword write

Matched archive `ps2-game-hle(20261120-060334).zip` contains the five
expected diagnostic reports and three P6 PPM captures from a single
Windows replay. The preceding DMA4 CHCR word-read fault at IOP
`0x0005B450` has cleared. The current replay reaches **41,231 EE
dispatches**, **16,325,205 guest instruction attempts**, reuses
**421 cached LLVM functions**, and commits **29 RPC commands**.
Six original IRX module-start threads complete and guest ARKD
registers servers `0x500..0x503`. All three snapshots (display
512x448, upload 1024x64, WebGPU 512x448) remain fully black.

**Exact stop:** guest IOP thread 9, PC `0x0005B5D4`, opcode
`0xA4A30000` (`sh v1, 0(a1)`), KSEG1 address `0xBF8010C4`
(physical `0x1F8010C4` = primary DMA4 **BCR**), 16-bit write,
`v0=0xBF8010C6` (address of BCR upper halfword), `a0=1`,
RA `0x0005B514`, `memory_fault=4` with **one genuine guest
RPC still pending**. The earlier `WaitSema(0)` diagnostic recurs,
with no verified producer or resolution. No guest ARKD method-`0x500:1`
completion, genuine SIF0/DMA5 reply, nonblack display or game boot
has been observed.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp`
decodes DMA registers into 32-bit MADR/BCR/CHCR/TADR words, extracting
the correct byte or halfword from a read and merging 8-/16-/32-bit
writes using a little-endian lane mask, preserving untouched bits.
DMA4 and DMA7 use independent primary/secondary channel banks.
Thus `SH` at `0x1F8010C4` must preserve BCR high 16 bits;
`0x1F8010C6` addresses the upper halfword of the *same* BCR.

**Coherent batch:** extend only the two previously verified DMA4/DMA7
banks to aligned 8-, 16- and 32-bit access, with per-lane read/write,
unaltered opposite halfword, independent sound-DMA channels, exact
R3000 `SH` guest regression at `0x0005B5D4`, invalid-width/
unaligned/bank-boundary rejection, no-fake-DMA/IRQ and cold reboot
checks. Other MMIO width rules remain unchanged. Programming CHCR.STR
still does **not** copy sound samples, complete a transfer, clear
STR, fabricate DICR flags, raise IRQ3 or release an EE semaphore.
The actual guest DMA sound-data movement and completion producer
remain unimplemented until grounded by a genuine transfer request.

**Next acceptance:** all CI checks on the exact new branch HEAD,
followed by a fresh matched Windows archive confirming this original
BCR write advances and whether the next authentic blocker is a
real IOP↔SPU2 RAM transfer or another guest operation.

## 2026-11-20 06:13:34 — DMA4 BCR store cleared; ARKD method 1 stalls on IOP wait

The matched archive `ps2-game-hle(20261120-061334).zip` contains all
five expected diagnostic files plus three P6 GS/WebGPU captures. The
original guest DMA4 BCR halfword store no longer faults. The EE reaches
the **100,000-dispatch limit** after **16,912,906 guest instruction
attempts** with **421 cached LLVM functions** and **29 committed
RPC commands**. The original six IRX module entry threads completed
and real ARKD servers `0x500..0x503` registered. All three captures
remain entirely black. Guest ARKD method `0x500:1` is **still pending**:
there is no real guest handler finish trampoline/SIF0->EE DMA5 reply.

Unlike the preceding four matched replays, **no new IOP unhandled
syscall or memory fault occurs**. The EE repeatedly visits
`0x00178DE8` and `0x001BD58C` until the dispatch limit. The
EE-cycle-based IOP scheduler continues thread 9 for at least 128
bounded turns of 256 genuine R3000 instructions each, but the
RPC remains pending. `WaitSema(0)` is still reported once during
this run at IOP PC `0x000147A8`; its producer is unverified.
The archived logs do not show the actual DMA4 CHCR/STR register
state at the final stall, so **an uncompleted DMA is a hypothesis,
not yet a proven guest dependency**.

**Primary reference:**
`ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp` implements IOP DMA4
and DMA7 with MADR/BCR/CHCR, and calls the actual
`spu2::dma()` from `start_dma()`. Its
`runtime/src/iop/spu2.cpp` moves real little-endian halfwords
between primary IOP RAM and shared SPU2 sound RAM at the guest
TSA, wrapping at the 1M-word sound RAM boundary. Direct DMA
completion is then scheduled after `64 + words/4` IOP cycles.
A stopped transfer cannot complete. AutoDMA uses a different
streaming input ring and cannot be faked using the direct path.

**Coherent HLE-first batch:** Connect the real IOP CPU RAM to
the existing guest DMA4/DMA7 register banks only when the IOP
hardware is attached. Read the original BCR count and MADR,
preflight the complete physical RAM span, copy actual guest
bytes to/from the two cores' shared sound RAM, update the
selected TSA, and raise SPU2 IRQ9 only if the original
enabled IRQA is crossed. The direct DMA completion occurs
after the reference-derived IOP instruction-clock interval:
only then can CHCR.STR clear and the actual DICR/DICR2
channel flag and IRQ3 be asserted. A guest CHCR.STR clear
cancels the pending completion. Invalid RAM ranges,
zero-length requests and AutoDMA must remain requested
with no fabricated data, completion or interrupt.

**New regression fixtures** exercise both DMA channels and
directions with known *real guest RAM* data, SPU2 RAM
wraparound, TSA progression, precise delayed STR clearing,
DICR/DICR2 flag creation after actual copying,
cancellation, invalid physical memory and AutoDMA fail
closed. A bounded guest RPC scheduler snapshot now
records thread-PC + actual DMA4/DMA7 register states
every 64 turns and records the thread wait statuses once
if a guest RPC remains pending with no runnable IOP work.
This distinguishes genuine DMA blockage from a thread
waiting on a guest kernel object or missing IRQ dispatch.

**Acceptance:** all CI workflows must pass on the final
branch HEAD. A fresh matched local Windows replay is then
required to identify the next actual guest state and verify
whether this real DMA backend advances ARKD method `0x500:1`.
No sound synthesis/playback or kernel IRQ callback execution
is claimed here.

### DMA completion under an idle IOP kernel

The ordinary direct DMA completion is a **device deadline**, not
a guest thread instruction. If the real transfer has already copied
all bytes but the IOP driver blocks waiting for its completion,
the previously implemented kernel `has_guest_work()` incorrectly
returned false and stopped advancing the IOP clock. The kernel
now includes pending real SPU2 DMA events in `has_guest_work()`,
bounds idle progress by its provided IOP cycle budget, and updates
the physical hardware timer when advancing to the next actual
DMA deadline. The DMA4 regression asserts that a kernel with
no runnable guest thread still clears STR and sets the real
DICR/IRQ3 completion *only* when 64 + words/4 elapsed, and
does not keep the DMA event runnable after completion.
AutoDMA, invalid memory and cancellation remain fail-closed.
This scheduling change still does not invoke guest INTRMAN
callbacks or send a synthetic ARKD SIF reply.


## 2026-11-20 06:35:50 matched retail replay and follow-up runtime parity

The complete matched Windows archive `ps2-game-hle(20261120-063550).zip`
was collected **on prior head `768a9e4c5e084b3f39d03f2adc8782e6e1b59533`**,
after the original DMA4/DMA7 sound-RAM transfer and idle-device scheduling
changes. It is NOT a validation of the later interrupt/scheduler patches.

- The EE reaches 100,000 dispatches, 16,912,906 attempted guest
  instructions, 421 cached LLVM functions and 29 committed SIFRPC commands.
  ARKD registers original guest servers `0x500..0x503`, but method
  `0x500:1` remains genuinely pending. Three GS captures are black.
- Real IOP DMA4 starts at MADR `0x000880ec`, BCR `0x00010010`,
  CHCR `0x01000201` and copies 64 bytes. Its 68-IOP-cycle deadline fires,
  clearing STR to CHCR `0x00000201`, publishing DICR `0x90900000`
  and I_STAT `0x00000008` (IRQ3).
- IOP ARKD thread 9 then blocks at `0x000147ac` with wait type 3
  (semaphore). A previous invalid `WaitSema(0)` at `0x000147a8`,
  RA `0x0007d648`, still returns authentic `-408`.
  The replay alone does not prove whether missing IRQ handler dispatch
  caused the pending semaphore.

### Implementation and source comparison after that replay

All changes remain on the single draft PR #145, not on main:

- `a9f8ef0`: individually track the two SPU2 core IRQ latches;
  assert IRQ9 whenever either core newly latches, even after the other
  core's I_STAT acknowledgement. Accept the reference's ignored
  SPDIF status halfword at `0x1f9007c2` for 32-bit OUT writes.
- `4661c61`: resolve enabled and pending DMA channels through
  DICR/DICR2 before ordinary IOP IRQ lines. Schedule the originally
  registered INTRMAN guest R3000 handler only after a real device
  event, acknowledge its source, preserve an interrupt context for
  QueryIntrContext, and respect the guest return value for ordinary IRQ
  masks. Extend the EE WaitSema guest-work path to pending calls as well
  as deferred binds. No synthetic iSignalSema, EE DMA5 or RPC reply.
- `7c5439e`: add regressions for SPU2 per-core IRQ9, SPDIF word
  stores, DMA handler and QueryIntrContext execution, plus a guest
  RPC handler requiring multiple IOP turns before a genuine response.
- `72c04a3`: keep a running IOP thread inside CpuSuspendIntr/
  CpuResumeIntr critical sections until it resumes, while allowing
  hardware DMA/timer cycles to progress during the masked interval.
- `93b1f23`: replace RPC-only EE-cycle scheduling with a bounded,
  carried-forward IOP clock at the reference's 8:1 EE-to-IOP ratio.
  Guest IOP work now progresses on elapsed EE time even without a
  pending ARKD call. This ratio is reference-compatibility timing,
  not validated per-instruction R5900 cycle accuracy.
- `3bacb44`: derive both IOP VBlank start and end edges from the
  existing GS scanout schedule, using the reference's 22-scanline
  end offset, instead of requiring manual end-edge test injection.

### CI and acceptance

The workflow changes in `3ee9831` run Windows MSVC and SHA256-pinned
Clang 22 independently, keep the combined `native-webgpu-windows` gate,
reuse Linux Lavapipe where installed, and cancel superseded PR runs.
`bced18e` divides the independent Python game-runner scenarios into
four exhaustive parallel CTest entries and retains three fast shards.

**Not yet established:** A full Windows retail replay at the new HEAD,
completion of ARKD method `0x500:1`, any nonblack GS capture, or real
sound output. The IOP critical-section and VBlank end changes also require
current-head CI validation before being regarded as accepted. LOADFILE
still replies when module `_start` is scheduled instead of after its
actual return, and native `timrman` / `sifman#32` coverage remains open.

Do not merge PR #145 without explicit authorization; when authorized,
squash-merge it once into `main`.
