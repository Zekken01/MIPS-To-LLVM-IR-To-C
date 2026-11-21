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
