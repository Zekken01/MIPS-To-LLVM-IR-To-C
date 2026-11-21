# PR #148 — matched USA retail graphics replay (21 November 2026)

Source archive: **sdbz-graphics-pr148.zip**, containing boot.log,
expansion.log, first-boot.json, translation-cache.json,
iop-disc-catalog.json, and all three CPU/GPU/upload PPMs.

Guest ELF: Super Dragon Ball Z USA SLUS_214.42, SHA-256
1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16.

## Authoritative final replay

Use the final stopped first-boot.json. Do not sum replay counters.

| Metric | Matched final result |
|---|---|
| EE dispatches | 3,656,670 |
| Guest instruction attempts | 94,621,123 |
| Final EE PC | 0x00175210 (host observation site) |
| EE report exit | UNSUPPORTED (4) |
| GS fields started | 18 |
| Committed GIF/GS batches and tags | 47 / 51 |
| VU1 XGKICK / GIF PATH1 | 0 committed batches |
| VIF1 DIRECT / GIF PATH2 | 7 batches, 11 tags, 133 input QWC |
| GIF DMA / PATH3 | 40 batches, 40 tags, 262,312 input QWC |
| PATH3 image payload | 16 events, 262,144 image QWC |
| Committed sprites | 6, all black RGB, all untextured |
| Native GPU draw count | 6 |
| Supported scanout / successful WebGPU presents | 16 / 16 |
| Host-poll skipped GS fields | 2 |
| CPU display | 512x448, zero nonblack pixels |
| Native WebGPU display | 512x448, zero nonblack pixels |
| CPU/GPU display same source and equal digest | true / true |
| Upload display | 1024x64, zero nonblack pixels |
| Runtime IOP module loads / RPC commits | 7 / 46 |

All three PPM images were inspected: all are entirely black.
Six valid GPU draws and sixteen successful native present calls
are real, but show only the *current black game output*, not the
first visible rendered frame.

The new execution advanced beyond the earlier unsupported VU0
VRSQRT at 0x00106988 and VMULq at 0x00106990. That proves
code-reach progression, not bit-exact vector arithmetic.

## True first blocker: guest IOP R3000 thread 81

The final boot.log recorded, before the EE unsupported stop:

    IOP clock guest execution stopped: turn=3470595
      thread=81 reason=3 pc=0x001cdf78 opcode=0x6e736168

IOP e_kernel_stop reason 3 is unsupported_instruction.
Little-endian word 0x6e736168 contains ASCII-like bytes "hasn".
It is not evidence of a missing legitimate R3000 instruction.
A corrupted guest function pointer, invalid return, callback entry,
or overwritten RAM are possibilities, but the supplied archive has
no last-eight-PC/thread-origin/stack window proving any one cause.

**Do not misattribute this to the EE INTC load at 0x00175210.**
The host sets EE UNSUPPORTED when the *IOP* stops; the EE's next
LW would have read already implemented INTC_STAT (0x1000f000).
No fake interrupt, no skipped opcode, and no synthetic device
completion is justified.

ARKD RPC server 0x503 method 257, real guest semaphores, and IOP
thread scheduling progressed before the failed R3000 instruction.

## Expansion and cache provenance

Ten outer expansion pass footers appear in expansion.log.
Passes 1–9 each report new_functions=128; pass 10 reports
new_functions=14, replay=8 and final stop 0x00175210.
A per-pass footer is not a final cache inventory delta.
Final translation-cache.json includes **1,167 IR hashes**.
Never sum final boot metrics across the expansion replays.

## Implemented follow-up batch

In the same draft PR #148, new *host-only* stopped-thread diagnostics
are being added without guest-visible changes:

- Bounded recent fetched R3000 PC/opcode ring per saved IOP thread,
  including the actual unsupported word, preserving context switches.
- Host-only thread creation origin, source/entry/module/IRQ/alarm
  classification, saved GPR/RA/SP, stack allocation and syscall history.
- Read-only RAM code windows around the invalid PC, thread entry, RA,
  and SP, with no diagnostic MMIO access.
- A one-time IOP stop report and an m2_iop_guest_fault JSON member
  for matched replay attribution.
- A focused regression executing exactly 0x6e736168, preserving its
  fail-closed unsupported outcome and verifying snapshot immutability.

The next matched replay should distinguish a bad initial thread entry
from an actual jump/return into data, enabling a causal kernel/IRX fix.
No first rendered game frame has been claimed.

The PR must stay draft and unmerged. Eventual merge is squash-only
after the user explicitly authorizes it.

## Follow-up matched archive: callback registration source

The user's second full graphics archive, `sdbz-graphics-pr148(1).zip`,
repeated the same final stopped replay: EE PC `0x00175210`,
3,656,670 dispatches, 94,621,123 guest instruction attempts,
18 GS fields, 16 successful native presents and six untextured
black sprite draws. All three captures remain black.

The previously implemented `m2_iop_guest_fault` report identifies
**thread 81 as a newly scheduled hosted callback**, not a routine
that branched into text after executing legitimate code.
Original entry `0x001cdf70`, module ownership `7`, module
load `PADMAN.IRX`, GP `0x001d5ff0`, SP `0x001d6cf0`,
callback-return RA `0x00013fe0`. Its entire fetched
history is:

```text
0x001CDF70  0x20746579  "yet "
0x001CDF74  0x20666973  "sif "
0x001CDF78  0x6E736168  "hasn"  <-- unsupported R3000
```

This string is in the loaded PADMAN allocation, but is not code.
No corrupt return is needed to explain the direct entry. The
registration producer is not yet identified in the supplied archive.

The PADMAN static import table includes VBLANK ordinals 8 and 9,
making a bad VBLANK registration plausible, but source attribution
requires registration evidence. The existing VBLANK handler uses
`a2` as the callback pointer, which matches both PS2SDK and the
primary `ps2-web-recomp/SDBZ` reference. Do not swap arguments
without a guest-side explanation.

### New coherent host-only provenance batch

- Preserve the original callback registration source: VBLANK start,
  VBLANK end, or CDVDMAN read; registration thread, syscall PC,
  RA/GP and original A0–A3.
- Preserve the last eight fetched R3000 instructions **from the
  registering thread** in a bounded ring, copied into the deferred
  callback thread without affecting its guest registers or execution.
- Record bounded registration events in the boot log, and include the
  carried callback registration origin in `m2_iop_guest_fault`
  when a guest callback actually fails.
- Add a regression for deferred callback provenance alongside the
  existing exact-unsupported-opcode failure test.

The IOP guest still executes its provided callback pointer.
A bad pointer still stops fail-closed. This batch does not change
IRX relocations, import ordinals, guest scheduling, DMA, VBLANK,
register values, or fabricate work. The next matched replay can
identify the original registration call, after which an evidence-backed
fix can be selected.

## Third matched archive: definitive stub collision and fix

The third uploaded full replay, `sdbz-graphics-pr148(2).zip`, reproduces
the **same authoritative stopped counters** (3,656,670 EE dispatches,
94,621,123 reached attempts, 18 GS fields, 16 successful native
presents, six black untextured sprite draws, and black CPU/GPU/upload
captures). The new callback registration evidence is decisive:

- `m2_iop_guest_fault.callback_registration.source = 3`:
  `cdvdman` `sceCdCallback`, registered by thread 54 at the guest
  service syscall PC `0x000187a8`, RA `0x001cd888`, A0
  `0x001cdf70` (ASCII `"yet sif hasn't..."`).
- The boot log has an **earlier** registration by PADMAN thread 53,
  using the **same** string address and the same HLE import stub,
  with RA `0x001cd7c4`. No guest fault occurred until the host
  subsequently completed a genuine disc read and dispatched this
  mistakenly registered address.
- The static `iop-disc-catalog.json` lists PADMAN's actual imports:
  **`sifman` #5 is present; `cdvdman` is absent**.
  The guest's real call went through a resolved SIFMAN import stub.
- Inspecting the HLE sources shows an exact memory collision:
  `sifman` #5 = `0x18700 + 5 * 0x20 = 0x187a0`, while
  old `cdvdman` #37 = `0x18300 + 37 * 0x20 = 0x187a0`.
  Similarly `sifman` #6 and `cdvdman` #38 both used `0x187c0`.
  CDVDMAN installation overwrote the actual R3000 `sceSifInit`
  service stub with a `sceCdCallback` syscall stub. As a result,
  PADMAN's legitimate SIF initialization was routed to CDVDMAN,
  with its ordinary diagnostic-string pointer in A0 being mistaken
  for a callable guest address.

**Causal fix:** relocate every CDVDMAN ordinal except the explicitly
fixed `sceCdSearchFile` #10 to the disjoint low IOP reserved range
`0x19000 + ordinal * 0x20`. The highest currently exported
CDVDMAN stub (#75) ends below `0x19980`, outside the existing
SIFMAN `0x18700` range, SIFCMD `0x18b00` range and the
`0x18f80` RPC-return trampoline; the guest module allocator
starts at `0x20000`. The old `sceCdSearchFile` #10
address remains `0x18200`.

**Regression:** compose KERNEL HLE + SIFMAN + SIFCMD + CDVDMAN in
both orders, check uniqueness and actual five-word R3000 service
selectors for all currently exported stubs, and execute a synthetic
non-copyrighted IRX whose original imported `sifman` #5 function
must initialize real IOP SIF DMA register state rather than invoke
CDVDMAN. This covers both the original `0x187a0` bug and the
previously latent `0x187c0` collision.

This fix is neither an exception bypass nor a fabricated interrupt,
RPC reply, DMA event, or graphical frame. It restores existing guest
SIFMAN service dispatch to the previously linked correct function.
A fresh **post-fix** retail replay is required before claiming
progress beyond thread 81 or a visible image.

## Fourth matched archive: after SIFMAN/CDVDMAN stub fix

Uploaded `sdbz-graphics-pr148(3).zip` is the first post-fix Windows
full graphics replay. Its `first-boot.json`, `expansion.log`,
`boot.log`, translation cache, catalogue and all three PPMs match.

| Metric | Previous IOP-stopped replay | Post-stub-fix replay |
|---|---:|---:|
| EE dispatches | 3,656,670 | 3,981,062 |
| Guest-instruction attempts | 94,621,123 | 1,847,424,953 |
| Guest GS fields | 18 | 375 |
| Native WebGPU successful presents | 16 | 268 |
| GPU committed GS draws | 6 | 255 |
| Committed GIF tags | 51 | 1,040 |
| VIF1 DIRECT / PATH2 batches | 7 | 256 |
| GIF DMA / PATH3 batches | 40 | 289 |
| VU1 XGKICK / PATH1 batches | 0 | 0 |
| IOP RPC commits | 46 | 292 |
| Cached native guest IR modules | 1,167 | 1,285 |
| Guest IOP failure | Thread 81 invalid text opcode | **None** |

The final `m2_iop_guest_fault` is `null`, and the boot log contains
no new guest unsupported IOP instruction, load/relocation failure or
fatal callback fault. The PADMAN misroute is resolved: the new run
enters genuine SIFMAN and survives to hundreds of IOP RPC and GS
field events.

**Visible image remains unverified.** CPU and WebGPU 512x448 captures
match byte-for-byte and have zero nonblack RGB pixels; the 1024x64
upload capture is also black. The GS committed 255 sprite pairs, all
untextured with zero RGB. The absence of a visible frame after 375
fields is not itself proof of missing GPU functionality, especially
because PATH1 has not committed any event.

### Exact stop classification

The authoritative final report ended with `HOST_YIELD (6)`, not an
unsupported opcode or crash, at EE PC `0x00175210`. The 4,000,000
configured dispatch limit was not reached: dispatch count was
3,981,062. The native runner had a 1,000,000-guest-instruction
loop-header watchdog but the `--loop-yield-resumes` option was not
forwarded by `run_ps2_game.bat`, so its CLI default was **zero**.
Thus the first translated-loop HOST_YIELD ended execution. It must
not be described as a fresh EE INTC MMIO blocker: the current EE PC
is again adjacent to the modeled `INTC_STAT` read.

`m2_runtime` pinpoints two dominant native dispatch entry PCs:
`0x00178de8` (1,815,980 visits) and `0x001bd58c`
(1,813,916 visits), with 1,813,916 cycles through each
direction of their edge. These two entrypoints account for
approximately 91% of all observed dispatches. The guest is
nonetheless continuing to run real IOP RPC, DMA/GIF and VBlank work:
a hot loop is **not** a demonstrated deadlock.

The one **outer** expansion pass printed `replay=78`,
`new_functions=118` and elapsed=1,221.55s. This is a per-pass
footer, not 78 outer passes. The final cache includes 1,285
`ir_hashes`. Preserve it; no more IR reconstruction is required
for these host-side diagnostic improvements.

### Host-only improvements for next long replay

- Runner supports optional environment
  `SDBZ_DISPATCH_TRACE_EVERY=N` (positive integer). Only the
  *verbose per-dispatch stdout text* is sampled. The first 64,
  every Nth and all non-ordinary or non-successful dispatches
  remain. CTest, all actual guest dispatches, recent JSON history,
  hotspots, hardware state and timing boundaries remain unsampled.
- `run_ps2_game.bat --full` defaults this cadence to **1024**;
  set `SDBZ_DISPATCH_TRACE_EVERY=1` to restore the old full trace.
  The previous 4-million-line `boot.log` was 293 MB uncompressed.
- The batch script now forwards the existing
  `--loop-yield-resumes` option via `LOOP_YIELD_RESUMES`,
  default **16**, preserving a hard bounded watchdog and
  allowing the already-committed guest state to resume at native
  safepoints. Set it to zero for the original immediate stop.
  It creates no synthetic cycle, IRQ, RPC result, DMA completion,
  GS field or frame.

These options are entirely host/launcher-side; do not delete the
existing translation cache or regenerate guest LLVM IR just to
change logging or native-loop resume counts. Use a larger explicit
`MAX_DISPATCHES` on the next run if the goal is to observe activity
beyond the old four-million-dispatch limit.

**Next evidence gate:** a new matched Windows full graphics replay
after these bounded knobs, with the existing IR cache retained.
Do not claim that boot reached a visible screen until a real,
nonblack CPU/GPU capture is verified.

## Fifth matched archive: cumulative native HOST_YIELD cap

The full archive `sdbz-graphics-pr148(4).zip` is the post-change
retail replay with `MAX_DISPATCHES=6000000`,
`LOOP_YIELD_RESUMES=16`, native watchdog 1,000,000,
`SDBZ_DISPATCH_TRACE_EVERY=1024` and cached translation intact.

**The stop is a host watchdog configuration limitation, not a newly
reached missing game opcode.** The final report shows:

| Metric | Previous replay (4) | Fifth replay |
|---|---:|---:|
| EE dispatches | 3,981,062 | 3,667,736 |
| Guest instruction attempts | 1,847,424,953 | 157,999,642 |
| EE GS fields | 375 | 31 |
| Window presents succeeded | 268 | 25 |
| Native GPU GS draws | 255 | 15 |
| Committed IOP RPC events | 292 | 51 |
| Guest IOP fault | null | null |
| EE exit | HOST_YIELD | HOST_YIELD |
| Cached LLVM IR modules | 1,285 | 1,285 |

`first-boot.json` and the final replay `boot.log` agree on
`dispatches=3667736`, `ee_cycles=157999642`,
`scheduler_yields=16`, final EE PC `0x00175210`,
`INTC_STAT=0x4`, `INTC_MASK=0x804` and
`m2_iop_guest_fault=null`. The actual 6-million-dispatch
budget is **not** reached. All three PPM captures remain black;
CPU and native-GPU display captures are pixel-for-pixel identical,
while the upload PPM is also black. PATH1/VU1 XGKICK still has
zero accepted batches; PATH2 committed 16 batches and PATH3
committed 49. The final `translation-cache.json` still contains
1,285 `ir_hashes`; the one outer expansion pass appended no
functions.

The boot log has seventeen actual
`0x00175204 -> 0x00175210 HOST_YIELD` transitions,
of which the first sixteen were resumed. The first two happened
at dispatch 10,426 and 10,514. There was then substantial
genuine EE/IOP/GS activity until the next native loop watchdog
at dispatch 3,639,873, followed by later watchdogs while
fields and GIF register writes continued. The previous runner
counted all sixteen resumes cumulatively across the *entire*
boot, so the seventeenth was rejected unconditionally at
dispatch 3,667,736, regardless of intervening progress.
This explains the new early stop; the smaller field/graphics
counts must not be described as a rendering regression.

### Causal host-only correction

New `ee::c_host_loop_yield_budget` preserves a strict finite
`LOOP_YIELD_RESUMES` allowance of sixteen **while no newly
observed real progress occurs**, but reopens a fresh bounded
window when at least one independently recorded GS field,
committed IOP RPC sequence, or GIF DMA register write
changes between watchdog events. Guest instruction counts,
ordinary dispatches, host wall time, and the host's own
yield counter do *not* qualify as progress. An unchanged
loop still stops after the configured number of resumes,
while setting `LOOP_YIELD_RESUMES=0` stops immediately.

This is observer-only host scheduling: no guest-visible PC,
GPR, timer, INTc state, DMA completion, callback, RPC result,
GS packet or frame is fabricated. CTest
`ee_host_loop_yield_budget` tests both fail-closed stationary
loops and genuine GS, GIF and RPC progress, including
the previous cumulative-cap failure.

`run_ps2_game.bat --full` retains the native watchdog,
the explicit user's 6-million-dispatch budget and
the low-volume verbose text cadence. The guest IR cache,
`run_game.py` cache schema and fingerprints remain
unchanged.

**Next gate:** a matched retail replay at the corrected
branch head to determine whether later boot traffic and
nonblack graphics appear. Neither a visible first frame
nor an EE/IOP timing-parity result is yet established.

## Sixth matched archive: test the memory-card/controller hypothesis

The screenshot provided with `sdbz-graphics-pr148(5).zip` shows
**PCSX2 running SDBZ USA** at the original startup warning:

> No memory card (PS2) inserted in MEMORY CARD slot 1 or MEMORY
> CARD slot 2. A memory card (PS2) with at least 110KB of free
> space is required to create a Super Dragon Ball Z save data.

The screenshot is a **reference presentation**, not an image of
the native LLVM-IR runner. It makes a controller-confirmation or
memory-card-init gate a worthwhile hypothesis, **not yet proof** of
which path the native guest is executing. The native runner does
not have a persistent virtual memory card, and its SIO2
controller starts with a neutral, unpressed virtual pad in port 0.

### Exact evidence in sixth matched replay

All eight members are from the current batch. The authoritative
final `first-boot.json` reaches the full configured 6,000,000
native dispatches, **12,737,447,273 EE guest instruction
attempts**, 2,588 GS fields, 1,826 successful native WebGPU
presents, 1,803 committed GPU draws and 1,804 progress-aware
HOST_YIELD resumes. **Exit `RETURN(0)` at the dispatch budget**
is not a fatal IOP/EE opcode. The final IOP fault object remains
`null`. The existing translation cache retains **1,285
guest IR modules**; the new outer expansion pass did not add any
functions.

GPU/CPU captures of 512x448 and the 1024x64 upload remain all
black. Both display captures match. All 1,803 committed sprite
kicks are untextured with zero nonblack RGBA/RGB provenance,
and VU1 PATH1 has zero accepted XGKICK batches. This is not a
verified rendered memory-card warning or main menu.

Crucially, the actual RPC timeline separates controller, memory
card and Arika file-service work:

- `SIO2MAN.IRX` loads at RPC sequence 13;
  `MCMAN.IRX` at 14; `MCSERV.IRX` at 15;
  `PADMAN.IRX` at 33. Loaded modules are evidence of module
  thread creation, not actual controller or memory-card bus I/O.
- PADMAN RPC client binds `0x80000100` and `0x80000101`
  and issues **four method-1 calls to server `0x80000100`**
  at sequences 36-39. The original PADMAN service runs via
  native guest IOP threads, and no invalid callback recurs.
- The main recurring operation is **1,796 RPC calls to server
  `0x503`, method 2**. This server belongs to the
  ARKD_DVD / Arika guest IOP RPC family `0x500..0x503`,
  as proven by repo's prior HLE parity audit and the server
  registration/guest handler. It is **not** a controller or
  memory-card server. Do not interpret its repeated calls as
  waiting for a user's X press without guest-code evidence.
- No `0x80000400` or `0x80000480` MCSERV RPC appears
  among the final 1,840 committed SIFRPC observations; the
  memory-card driver has not been proved to service a request.
- The current log has SIO2 hardware snapshots only through
  2,000,000 EE dispatches; all seven report **zero CTRL.start
  events, zero successfully processed SIO2 packets, and zero
  completed SIO2 transfers**. There is no final SIO2 counter
  in this archive, so we cannot assert no controller poll
  happened later in the 6-million-dispatch session.

### Read-only diagnostic added

`first-boot.json` now includes optional
`m2_iop_sio2` containing an actual `ctrl_starts`,
`packets`, `completed_transfers`,
`transfer_requested` and `pending_tx_bytes`
snapshot, or `null` when the guest IOP is absent.
This is written at both periodic and final reports, using
the existing IOP hardware's *actual observed transactions*.
It does not create a controller read, supply a fake
button press, simulate a virtual memory card, claim a
successful SIO2 transfer, or change any guest cycles.

**Next acceptance gate:** determine whether SIO2 ever
transfers a real controller packet in a post-change
six-million-dispatch replay. If counters remain zero,
investigate original PADMAN/SIO2MAN guest scheduling,
input task state and why no controller poll is produced;
adding host input cannot influence a guest that never
reads the device. If transfers are present but neutral,
implement a deliberate, testable host gamepad/input mapping
against the genuine SIO2 protocol and check whether an
actual button edge advances the native guest. Separately
compare MCSERV RPC traffic and rendered GS primitives;
do not assume a UI prompt is running when captures
remain black.

Everything is a host-only reporting change. The existing
1,285 cached LLVM modules must be preserved.

## Seventh matched archive: first final SIO2 input-bus evidence

The resent `sdbz-graphics-pr148(7).zip` contains the eight
normal replay artifacts and is byte-for-byte identical to
`sdbz-graphics-pr148(6).zip`. It is the **earlier
post-initial-SIO2-telemetry replay**, not a replay of the
new original-USA-ELF xref analysis.

The stopped `first-boot.json.m2_iop_sio2` confirms:

| Observed original IOP bus event | Actual count / state |
| --- | --- |
| SIO2 CTRL.start writes | **1** |
| SEND3 protocol packets processed | **2** |
| Completed SIO2 transfers | **0** |
| Host SIO2 request still outstanding | **true** |
| Queued unprocessed TX bytes | **0** |

All SIO2 checkpoints through 2,000,000 EE dispatches were
zero. This **final** capture proves later genuine IOP
SIO2 traffic. It does **not** establish that the caller
has already received pad button states or that a
memory-card warning is displayed. The virtual pad starts
connected but neutral; no virtual memory card is present.

No further game progress is established relative to the
sixth replay: exactly 6,000,000 EE dispatches,
12,737,447,273 attempted guest instructions,
2,588 GS fields, 1,826 successful native WebGPU
presents, 1,803 untextured **black** sprite draws,
zero PATH1 XGKICK, no IOP guest fault and all three
PPM captures still black. `translation-cache.json`
is identical, with 1,285 IR modules. The run terminates
at the configured dispatch limit, not at a new IOP fault.

### Source audit: the exact incomplete SIO2 event

`src/iop/iop_hw.cpp` processes SEND3 packets through the
actual IOP TX FIFO or DMA11. The packet handler can
queue replies and advance `m_sio2_index` even before
a subsequent `CTRL.start`. However
`run_sio2_transfer()` returns false on several
distinct hardware conditions, including:

- No actual new packet and no buffered reply.
- Invalid or unbacked DMA11 source, or DMA12
  destination/geometry.
- A required packet or reply still missing.
- Too few DMA12 output words or failed guest RAM write.

On such a rejection it leaves request pending, does
not clear DMA11/12 STR, and does not raise IRQ17.
That is consistent with the observed counters,
but the **specific failing predicate cannot be
recovered** from the old five SIO2 report fields.
Two processed packets plus no pending TX bytes
do not prove that the RX queue was empty.

Compared with primary reference
`twist84/ps2-web-recomp:SDBZ/runtime/src/iop/sio2.cpp`,
the reference `run_transfer()` raises IRQ17 at
`CTRL.start`, including empty-PIO cases, whereas
the current stricter native HLE rejects empty or
incomplete requests. This is a specific parity
difference to examine; do not equate that alone
with permission to fabricate an interrupt or
make a false DMA completion.

### Read-only rejection snapshot added after this replay

- The genuine SIO2 preflight now records
  `last_rejection` and `rejected_starts` for
  exact failed CTRL.start conditions without
  changing guest state.
- The stopped and periodic `m2_iop_sio2`
  objects additionally include the actual
  `queued_rx_bytes`, `packet_index`,
  `ctrl_word`, `stat_word`,
  first four `send3` descriptors and
  DMA11/DMA12 MADR-BCR-CHCR triplets.
- The original no-completion/no-IRQ invariant
  remains intact until sufficient authentic
  guest evidence identifies the actual mismatch.
- `iop_hw_test` extends the synthetic PIO/DMA
  fixtures to verify incomplete packets,
  empty-CTRL request, and invalid DMA11 source
  are correctly diagnosed, are fail-closed,
  and successful transactions reset the
  last rejection.

The next completed replay will distinguish whether
the original guest exhausted the PIO RX FIFO
before CTRL.start or attempted a genuinely
incomplete DMA11/12 transfer. Then make a
targeted parity correction with an exact guest
transaction regression if warranted.

Only host IOP hardware diagnostics, runner JSON,
smoke tests and this document are changed. No
original executable, guest LLVM IR, function
discovery metadata, translation cache or guest
input is modified.
