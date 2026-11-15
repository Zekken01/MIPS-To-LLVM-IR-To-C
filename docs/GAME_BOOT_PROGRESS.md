# Native game boot research — Super Dragon Ball Z (USA)

This is a **bounded, experimental EE native-code runner**, not a PS2
emulator and not yet a playable game. No copyrighted game ELF is stored here.

## Getting started

Build the existing CMake project and install `llvmlite` into the Python
environment used by CMake and the runner:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
python3 tools/run_game.py /path/to/SLUS_214.42 \
  --build-dir=build --work-dir=build/sdbz \
  --hle-startup --kseg-ram --trace --auto-expand=16 \
  --report=build/sdbz/first-boot.json \
  --max-dispatches=128 --timeout-seconds=60
```

To continue with existing translations instead of rebuilding the initial
function set, use `--no-recompile --auto-expand=N`. This reuses the
existing linked library and metadata from the same ELF, and only compiles and
relinks if execution reaches a new guest function PC. The ELF path and entry
are validated against the saved metadata. Do not use this after changing the
ELF or compiler/lifter semantics without a clean recompilation.

`tools/run_game.py` invokes `ee_recomp`, verifies each LLVM IR module,
emits native objects, links a guest-PC dispatcher library, and starts
`ee_game_runner`. The game binary remains local. `--auto-expand=N`
compiles one newly reached guest function, relinks, and replays from entry,
up to N times; this is not an instruction-step emulator. `--max-dispatches`
counts *native function entries*, not MIPS instructions. A replay timeout
catches guest loops that do not return to the dispatcher.

On Windows, `--pdb` requests a matching
`game-translated.pdb` beside `game-translated.dll` using Clang and
the LLD MinGW linker. This passes `-g -gcodeview` for runtime and
dispatcher C++ compilation and `-Wl,--pdb=...` at link time.
The DLL and PDB are checked after linking and recorded in the
`.dll.link.json` manifest. A GCC/binutils-only toolchain is not
supported for PDB output. To generate a PDB without rebuilding the
guest IR **or launching the game**, use `--no-recompile --pdb --link-only`
with the existing `--work-dir`. This relinks the DLL using already-generated
guest objects and writes `game-translated.dll` and
`game-translated.pdb` side by side. Generated
guest functions have native symbols but do not yet carry source-level
LLVM debug metadata, so a PDB does not provide reconstructed MIPS/C++
source lines; it primarily makes native symbols and compiled runtime
information available to Windows debuggers. If you change the
`--emit-opt` setting, perform a fresh compilation first.

Native host-object generation defaults to `--emit-opt=0` for fast iteration.
To compare LLVM backend optimization, run with `--emit-opt=2` (supported
levels 0–3). This option changes **host machine-code optimization**;
it does not modify guest code, patch emulation, or supply PS2 kernel state.
An O0/O2 synthetic-ELF equivalence test checks that the guest's resulting
register state and exit behavior agree. For game comparisons, prefer
a separate `--work-dir` per optimization level to avoid overwriting
previously linked objects and to keep benchmark results reproducible.
When auto-expanding an existing work directory via `--no-recompile`,
the chosen optimization level is applied on the next relink.

`--report=FILE` produces a JSON report with the stop reason, COP0 exception
fields, all 32 low GPR halves, the instruction at EPC when available, and the
last 64 native dispatch transitions. Reports are written even when the guest
returns a non-zero blocked-execution status. Report history tracks **native
function dispatches**, not individual MIPS instructions. The report has a
`snapshot_kind` field: `stopped` after an orderly runner exit and
`in_progress` while code is still executing. An in-progress snapshot is
written at replay entry, every 16 dispatches, and on transitions into or out
of a registered syscall callback; this permits diagnosis when a hard replay
timeout kills a translated function without ever returning to the dispatcher.
The in-progress PC is the **last observed** PC, not necessarily the instruction
that was executing when the timeout occurred.
The report also contains a rolling `syscall_history` of the latest 32
installed guest-handler invocations, including the guest's A0-A3 arguments,
handler PC, resume PC, dispatch index, V0 return value, and whether the
handler returned. An unfinished entry has `returned: false`; its `v0`
value is not a valid result until the callback completes. This makes it
possible to identify repeated, unchanging syscall arguments without assuming
that repeated dispatches imply progress. With `--auto-expand`, each
replay overwrites the report, so its latest snapshot belongs to the most
recent attempted run. For timeouts, the Python driver prints the last saved
dispatch and PC if the report is readable. `--kseg-ram` opts into mapping the EE KSEG0/KSEG1 direct-RAM mirrors
onto physical RAM; it does not implement TLB, PS2 hardware MMIO, or device
side effects.

The runner initializes 32 MiB of EE RAM and a separate 16 KiB EE
scratchpad at `0x70000000..0x70003fff`, copies PT_LOAD segments, initializes
PC and SP and stops on exceptions, uncompiled PCs, exhaustion, or unsupported
host services. The scratchpad mapping follows the region boundaries in
`ps2-web-recomp/runtime/include/ps2/memory.hpp` and its memory resolver.
It supports bounded 8/16/32/64/128-bit loads and stores through the existing
generated-code memory helper ABI. The EE's VU0 physical memory apertures are
also mapped when the runtime has an `m_vu0_state`:
`0x11000000..0x11000fff` for VU0 micro-program memory and
`0x11004000..0x11004fff` for VU0 data memory. These addresses match
`ps2-web-recomp/runtime/src/memory.cpp`; the mapped micro-program array is
the same one used by `ee_vu0_fetch_micro_instruction`.
The map rejects cross-boundary reads/writes without silently wrapping.
The EE physical-address dispatcher also supports **opt-in** direct aliases:
`0x80000000..0xbfffffff` aliases physical `0x00000000..0x1fffffff`;
`0x20000000..0x3fffffff` aliases physical `0x00000000..0x0fffffff`.
This follows `ps2-web-recomp/runtime/src/memory.cpp::physical()`, but RAM
accesses remain bounded by the actual allocated RAM size rather than
silently mirroring unbacked physical addresses.

A host may now attach `s_ee_runtime::m_mmio_read` and `m_mmio_write`.
Within the physical EE hardware aperture `0x10000000..0x12ffffff`,
the dispatcher supplies the **physical address**, exact access width
(1/2/4/8/16 bytes), and 128-bit value. A 16-byte FIFO access is a
single callback, not two 8-byte callbacks. VU0 memory has priority.
Unimplemented devices return zero on reads and ignore writes by default.
The host can install device implementations through these hooks; the
native game runner now installs a minimal EE **INTC** model for
`INTC_STAT=0x1000f000` and `INTC_MASK=0x1000f010`. Its 32-bit writes
acknowledge status bits (write-one-to-clear) and toggle enabled-mask bits
(write-one-to-toggle), matching `ps2-web-recomp/runtime/src/hw.cpp`.
It raises/clears COP0 Cause.IP2 whenever a masked interrupt is pending.
The native runner's host MMIO bus now also exposes the DMA interrupt
status register `D_STAT=0x1000e010`. Its low 16 bits are write-one-to-clear;
the upper 16 bits are write-one-to-toggle channel masks. Bits 0..9 report
the ten DMA channels, and masked pending channels assert COP0 Cause.IP3.
These register rules come from `ps2-web-recomp/runtime/src/hw.cpp`.
Separate INTC and DMAC models preserve their own pending COP0 lines.
Devices must explicitly call `c_ee_intc::raise(cause)` or
`c_ee_dmac::raise(channel)` when a real hardware event occurs.
The native host also supports **normal-mode scratchpad DMA** for
`fromSPR` channel 8 (`0x1000d000`) and `toSPR` channel 9
(`0x1000d400`), using their `CHCR`, `MADR`, `QWC`, `TADR` and
`SADR` register offsets. Writing `CHCR.STR` from 0 to 1 in normal
mode transfers `QWC` 16-byte chunks between physical EE RAM and
the 16 KiB scratchpad. `SADR` wraps on the scratchpad boundary;
`MADR` advances, `QWC` clears, `STR` clears, and `D_STAT`
latches the matching channel completion bit, asserting IP3 if enabled.
The transfer behavior is based on
`ps2-web-recomp/runtime/src/dmac.cpp::run_normal`.
Unlike the reference's fast RAM mirroring, this experimental backend
rejects unaligned/unbacked RAM transfers atomically instead of copying
partially or pretending to complete. Other IPU/SIF device engines,
MFIFO, most DMA channels, and cycle-based DMA completion are not implemented.

The controller now also exposes **normal-mode device DMA** for VIF0
(channel 0, `0x10008000`), VIF1 (channel 1, `0x10009000`), and
GIF (channel 2, `0x1000a000`). The native host may install a
`c_ee_hw_bus::bind_device_transfer` callback which receives one
**complete**, contiguous, little-endian 16-byte/QWC stream and the
channel index. Channel 2 represents GIF PATH3, and VIF1 accepts only
RAM-to-VIF uploads (`CHCR.DIR=1`), not reverse GIF/GS image downloads.
Only normal-mode (`CHCR.MOD=0`) is supported for these channels.
On an accepted transfer, MADR advances, QWC and STR clear, and
D_STAT/COP0 IP3 report channel completion. A missing/rejecting callback,
unsupported mode/direction, or an invalid physical RAM span preserves
STR and the other channel registers, without pretending that the GPU
or VIF executed anything. The callback must reject an unavailable
consumer **before** committing side effects; the DMA host cannot undo
work performed by a callback that returns false.

The optional **GIF PATH3 parser** (`c_ee_gif_path3`) can now be
connected to channel 2 through `bind_device_transfer`. Based on
`ps2-web-recomp/runtime/src/gs/gif.cpp::walk`, it decodes GIFtags
(NLOOP, EOP, FLG, NREG and REGS), packed 128-bit GS register payloads,
reglist 64-bit register values (including odd-register padding), and
image/IMAGE2 quadword runs. PACKED tags with PRE emit a separate PRIM
register event; NREG=0 represents 16 registers. The parser preserves
state across separately completed DMA transfers when a GIFtag's payload
continues in the next transfer. An external consumer receives a single
ordered batch of raw events per DMA transfer, plus offsets into the
original input for image payloads, and must accept the **entire** batch
atomically. On rejection, the parser state stays unchanged and the
DMAC does not advance or raise completion. It does not infer GS register
side effects or perform rasterization; there is deliberately **no**
default event sink and the native game runner still leaves it unbound.

The opt-in **GS register-state sink** (`c_ee_gs_state`) can now receive
the PATH3 parser's complete event batches. Based on
`ps2-web-recomp/runtime/src/gs/gs.cpp::write/write_packed`, it captures
PRIM/PRE, RGBAQ, packed STQ and Q reset, UV, XYZ/XYZF (including
ADC routing), FOG, TEX0/TEX2 merging, per-context texture, frame,
scissor, depth, alpha and test setup, and the image-transfer registers
BITBLTBUF (0x50), TRXPOS (0x51), TRXREG (0x52) and TRXDIR (0x53).
GS now has its own **4 MiB VRAM**, separate from EE RAM and the EE
scratchpad. For PSMCT32, PSMCT24, PSMCT16, PSMCT16S,
PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
PSMZ32, PSMZ24, PSMZ16 and PSMZ16S host-to-local transfers
(TRXDIR=0), it decodes BITBLTBUF's 256-byte destination base units
and 64-pixel buffer width, TRXPOS's 11-bit destination coordinates,
and TRXREG's transfer rectangle. PSMCT32/24 use 64x32-pixel GS pages;
PSMCT16/16S use distinct 64x64-pixel pages with 16x8 blocks and the
reference's shared 16-bit column permutation. PSMCT16S selects a
different block permutation from PSMCT16. PSMT8 uses 128x64-pixel
8-KiB pages, 16x16 blocks and the reference's word/byte swizzle;
its page-row stride is max(1, DBW/2) after integer division.
PSMT4 uses 128x128-pixel, 8-KiB pages with 32x16 blocks and
the reference's 4-bit word/nibble permutation, using the same
minimum page-row stride rule as PSMT8.
PSMT8H, PSMT4HL and PSMT4HH share PSMCT32's 64x32-pixel
page layout and address permutation, but store their indices
in a 32-bit GS word's high byte, low nibble of that byte,
or high nibble of that byte respectively.
PSMZ32/24 use 64x32-pixel, 8 KiB pages with 8x8 blocks,
and PSMZ16/16S use 64x64-pixel pages with 16x8 blocks.
All four depth formats use their own block permutations from
the reference, sharing only their 32- or 16-bit color-column
swizzling tables. PSMZ16S differs from PSMZ16.
PSMCT32 writes four bytes per pixel; PSMCT24 writes three bytes while
preserving its GS word's high byte; PSMCT16/16S write two bytes while
preserving the adjacent halfword. PSMT8 writes one raw 8-bit palette
index per pixel into its swizzled byte position; PSMT4 writes
one raw 4-bit palette index into the selected half-byte,
preserving the other nibble. PSMT8H writes one upper-byte
index while preserving the bottom 24 bits. PSMT4HL/4HH
preserve the other 28 bits of the 32-bit destination word.
These uploads only store indices; they do not convert them to colors.
PSMZ32 stores four bytes per depth value; PSMZ24 stores the low
three bytes of a GS word while preserving its upper byte, and
PSMZ16/16S write two bytes while preserving the adjacent halfword.
Pixel coordinates and byte
addresses wrap at their respective 11-bit and
4-MiB boundaries, matching the reference implementation.

IMAGE/IMAGE2 quadwords can span separately completed GIF/DMA batches.
The transfer tracks completed pixels, ignoring trailing quadword padding
once the rectangle has been filled. The previous bounded 4 MiB raw-image
capture remains available for diagnostics; this is not a texture
decoder or rendered frame. PSMCT32 and PSMZ32 pixels cannot split across 16-byte
IMAGE quadwords; PSMCT24 and PSMZ24 retain incomplete 1- or 2-byte
pixels between event batches. Neither PSMCT16 nor PSMCT16S,
nor PSMZ16 nor PSMZ16S, requires
partial-pixel carry because each GIF IMAGE quadword holds eight
complete 16-bit pixels; PSMT8 has 16 complete one-byte indices per
quadword; PSMT8H also has 16 indices per quadword. PSMT4 and
PSMT4HL/PSMT4HH each have 32 packed 4-bit indices per quadword
(low nibble first). Odd-width rows do not introduce per-row padding;
indices remain packed in raster order across rows. Final RGBA
conversion and texture filtering, other unsupported PSM formats,
host-to-local HWREG is supported through GIF A+D register 0x54.
GS local-to-host
(TRXDIR=1) is supported through read_image and VIF1 reverse DMA.

Every event batch is applied to a temporary state copy and its VRAM
pixel writes are staged until **all** events are supported, input offsets
are valid and any image payload fits the capture bound. Unsupported
host-to-local destination formats (including reserved PSM 0x03),
zero destination buffer width, zero-size transfer rectangles and side
effects such as GS SIGNAL/FINISH/LABEL and TEXFLUSH, or IMAGE
without proper setup, reject the whole batch. Rejection preserves the
GS state, VRAM, and upstream GIF parser/DMA progress. The GS VRAM is
zero-initialized and cleared by the consumer's reset; this is a
deterministic host reset policy, not a claim about physical GS power-on
contents. Applications opt in by
binding `c_ee_gs_state::gif_events` as the GIF parser's event sink.
The native runner deliberately does not bind the parser or GS sink by
default, because a working GS renderer is not yet present.

This is **not** a VIF interpreter, a GS primitive rasterizer, GPU
presentation, device DMA chain engine for channels 0..2, MFIFO, or a
timed hardware queue. Only PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8,
PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
PSMZ32, PSMZ24, PSMZ16 and PSMZ16S IMAGE uploads have
GS VRAM effects;
real SDBZ image-stream and PCSX2 differential validation remain needed.


### GS HWREG host-to-local uploads (A+D register 0x54)

The GIF PATH3 state consumer now implements **HWREG (0x54)** writes
through PACKED A+D as eight little-endian image-data bytes, precisely
as in `ps2-web-recomp/runtime/src/gs/gs.cpp::write()`. HWREG is
valid only after a supported `TRXDIR=0` transfer has been configured.
It shares the same pixel decoding and swizzle staging as IMAGE/IMAGE2,
covering all thirteen color, depth, and indexed PSMs. For 24-bit
PSMCT24/PSMZ24, the unfinished 1-2 pixel bytes are carried across
HWREG writes, IMAGE qwords, and separate GIF DMA calls. Four-bit
indices are decoded low nibble first across odd-width rows. After
TRXREG's rectangle completes, surplus HWREG/image data is ignored
rather than writing extra pixels. A+D and IMAGE transfers in one GIF
event batch retain their original order, including VRAM updates
observed by a later TRXDIR=2 copy or TEX0 palette load.

HWREG writes take part in the existing transactional GIF and DMA
commit: a later unsupported GS register cancels all staged VRAM
pixels, register changes and pending carry. `m_hwreg_write_count`
and `m_hwreg_bytes` record cumulative HWREG input, but HWREG
does *not* increment `m_image_qwc` or append to the IMAGE diagnostic
capture buffer. Reset clears all counters. The current support is
the GIF A+D register path; a separate GS privileged-register/MMIO
HWREG input path is not asserted to exist.

### GS local-to-host downloads (TRXDIR=1)

The GS consumer now supports **TRXDIR=1** local-to-host image readback
from all 13 verified GS pixel formats. The source is selected using
BITBLTBUF.SBP/SBW/SPSM, TRXPOS.SSAX/SSAY and TRXREG.RRW/RRH.
`c_ee_gs_state::read_image(data,qwc)` returns 16*qwc bytes in GS
raster order, unswizzling the stored pixels. 32/24/16/8-bit values
are packed little-endian, and all 4-bit formats are low-nibble first.
PSMCT24/PSMZ24 readback retains partial pixel bytes across separate
DMA reads (a byte-oriented cursor), and a final, partial quadword is
zero-padded. Extra read requests after the rectangle complete also
return zeros, matching the reference's `render_read_image()` behavior.
Pixels and GS VRAM addresses wrap as with GS uploads; the download
does not alter the internal CLUT.

`c_ee_dmac::bind_device_download` connects an optional GS producer
to **VIF1 channel 1 reverse DMA (CHCR.DIR=0)**. The DMAC prevalidates
the entire EE RAM destination range, requests 16*qwc bytes from the
producer and writes them back to EE RAM only after the full request
succeeds. On missing/refusing producer or invalid RAM, MADR/QWC/STR,
the interrupt status and RAM remain unchanged. VIF1 DIR=1 still
uses the existing forward device consumer. Neither direction
implements VIF commands, FIFOs or cycle-accurate DMA scheduling.
The GS readback cursor advances only on successful reads. Host
applications can bind `c_ee_gs_state::dma_download` through
`c_ee_hw_bus::bind_device_download`; no default GS binding is added
to the native runner.

### GS local-to-local VRAM copies (TRXDIR=2)

The state-only GS consumer also executes **TRXDIR=2** at the GS
register write, after decoding BITBLTBUF source and destination
base/width/PSM, TRXPOS source/destination 11-bit coordinates and
DIR (bits 59..60), and TRXREG width/height. It supports all 13
verified PSMCT, PSMZ and indexed PSMT formats on both sides,
including unlike source/destination formats, with raw numerical
pixel values converted by the destination's existing write rules.
PSMCT24/PSMZ24 preserve destination high bytes, 16-bit formats
preserve adjacent halfwords, and indexed 4-bit/8-bit formats
preserve neighboring nibbles and high-byte lanes.

Like the `ps2-web-recomp` reference's `local_copy()`, the copy
reads every source pixel from a **frozen source snapshot** before
applying destination writes. Thus overlapping source/destination
regions behave deterministically. DIR bit 0 reverses the *source*
horizontal pixel selection; DIR bit 1 reverses its vertical selection.
Both source and destination coordinates wrap at 2048. GIF event-batch
ordering is preserved: earlier IMAGE uploads and earlier local copies
in the same batch are visible at a subsequent TRXDIR=2, but later
writes are not. All local-copy writes are staged and either
committed with the entire batch or fully rolled back if a later
event fails, including the copy counters and GS register state.
A safety bound of `k_max_local_copy_pixels` (4 Mi pixels
total staged per GIF batch) rejects oversized copies rather than
allocating unbounded temporary storage. TRXDIR=2 does not consume
GIF IMAGE payload and does not reload an existing CLUT snapshot.

Host-to-local uploads still use TRXDIR=0; device rendering,
other GS GPU behavior, and real SDBZ/PCSX2 differential validation
remain outstanding.

### Indexed CLUT snapshots (CSM1 and CSM2)

The GS state-only consumer now loads a **512-entry raw palette snapshot**
on supported TEX0/TEX2 CLD requests, rather than deriving an index's
color directly from live GS VRAM. CSM1 handles the PSMT8 index 3/4 bit
swap and 4-bit 8x2 entry layout. CSM2 reads TEXCLUT.CBW/COU/COV,
including page-spanning rows. Palette base CBP uses the same
256-byte GS VRAM units as the other GS buffers. Supported CPSM
storage formats are PSMCT32, PSMCT16 and PSMCT16S, using their
existing swizzles. Palette entries are returned as **raw 32- or
16-bit values**; 16-bit channel expansion, TEXA alpha expansion,
TCC/TFX, texturing filters, and rasterization are not implemented.

CLD=0 and CLD=6/7 do not load. CLD=1 forces a load; CLD=2/3
load and record CBP in one of two cached slots; CLD=4/5 skip the load
if the associated CBP matches, otherwise load and replace that slot.
These markers and CLUT contents are kept in transactional GS state,
including across GIF/DMA batches, and cleared by host reset.
TEXCLUT writes alone do not reload a palette. Images written *after*
a TEX0/TEX2 load do not silently change its snapshot; an IMAGE before
TEX0/TEX2 in the **same** event batch is visible to the palette load
using a palette-only staged-byte overlay. A later unsupported event
rolls back both VRAM and palette state.

\`read_clut_entry(slot)\` and
\`read_indexed_clut_value(context,x,y)\` are diagnostic read APIs,
not a GS renderer. Indexed reads support PSMT8, PSMT4, PSMT8H, PSMT4HL and
PSMT4HH, returning loaded raw palette words; all 4-bit modes
apply the TEX0.CSA bank selection. The indexed palette storage modes and load semantics here
follow \`ps2-web-recomp/runtime/src/gs/gs.cpp::load_clut()\`.
Explicitly unsupported CPSM values and formats with unknown CLUT
semantics reject CLD-triggered loads instead of inventing colors.
Real SDBZ/PCSX2 differential validation and full GS rasterization,
texture filtering, palette alpha treatment, the depth test and
depth rasterization, and other GS transfer directions remain outstanding.

`fromSPR` channel 8 also supports
**synchronous chain mode** (CHCR.MOD=1). It copies a preloaded QWC segment
when present, then consumes 16-byte DMA tags from scratchpad, selecting
the tag's upper-32-bit MADR and low-16-bit QWC. It updates CHCR tag bits,
advances SADR over the tag and data (wrapping at 16 KiB), and terminates
on tag ID 7 or on an IRQ tag when TIE is set. An END-tag completion
clears STR and asserts D_STAT channel 8 according to its enabled mask.
Unlike the reference runtime's streaming implementation, this backend
preflights the **entire** chain's physical RAM destinations before any
copy. Invalid spans or an unterminated cycle (limited to 4096 tags)
leave STR set, preserve channel registers, and do not partially copy
RAM or raise a false completion interrupt.

**toSPR** channel 9 now implements the reference's general DMA
chain mode (`CHCR.MOD=1`), including a preloaded QWC segment and the
following tag IDs: REFE (0), CNT (1), NEXT (2), REF (3), REFS (4),
CALL (5), RET (6) and END (7). DMA tags live in EE RAM at TADR,
carry QWC, destination/source-selection and IRQ flags, and can choose
inline payload or an aligned physical RAM reference. CALL/RET use two
address-stack registers (ASR0/ASR1 at channel offsets 0x40/0x50);
TIE-enabled IRQ tags terminate after their payload. Transferred
quadwords go to scratchpad, with SADR wrapping at 16 KiB.
TADR, MADR, CHCR tag/stack fields, QWC, STR, D_STAT and COP0 IP3 are
updated on successful completion.

As in the fromSPR implementation, all tag addresses and source RAM
spans are checked on a read-only pass before any scratchpad writes.
Malformed tags, unbacked RAM and nonterminating loops (4,096 tag
guard) leave STR set and do not fabricate completion. This differs
from real PS2 streaming side effects; differential ELF/PCSX2
validation remains necessary. MFIFO, cycle scheduling, and
device-side DMA interpretation remain unsupported.

The EE DMA control register `D_SQWC=0x1000e030` is now backed by the
shared hardware bus. For SPR channels 8 and 9 in `CHCR.MOD=2` when
`D_SQWC.TQWC=(D_SQWC >> 16) & 0xff` is nonzero, **interleave mode**
copies one quadword per RAM slot, skips `D_SQWC.SQWC=D_SQWC & 0xff`
RAM quadwords after each group of TQWC transfers, and always consumes
scratchpad source/destination quadwords contiguously, wrapping at 16 KiB.
The final group's skip is also included in the resulting MADR.
Both directions update MADR, SADR and QWC, clear CHCR.STR, and latch
channel completion via D_STAT/IP3. When TQWC is zero, the reference
falls back to ordinary contiguous normal DMA even if MOD=2.
As with normal DMA and fromSPR chains, **every RAM qword is
prevalidated** before any writes; an invalid sparse transfer leaves
data/registers intact and does not generate a false completion.
These synchronous rules follow
`ps2-web-recomp/runtime/src/dmac.cpp::run_interleave`; hardware
cycle timing remains unimplemented.
A translated native dispatch now samples pending, enabled EE COP0
interrupts **at mapped function-entry boundaries**. When an interrupt is
pending and IE/EIE/IM permit it, the existing exception helper sets
Cause.ExcCode=Int, EPC to the next guest entry PC, Status.EXL and the
configured BEV exception vector, without invoking the next guest function.
This also detects pending levels immediately after a translated ERET returns.
The synthetic PC-zero entry sentinel is excluded. A `max_dispatches` limit
can postpone a checkpoint until the next call; the pending level persists.
The runner reports the exception rather than misdiagnosing it as a missing
entry, but it cannot execute an unavailable BIOS handler.

This is **not** cycle-accurate EE interrupt delivery. A translated function
that runs indefinitely never reaches the next checkpoint; handling
interrupts within such code requires instrumentation and a timed scheduler.
The host does not yet provide device event timing, complete guest BIOS
interrupt handlers, or general GS/SIF/GIF/VIF behavior.

This still does **not** implement MFIFO,
TLB translation, VU1, guest address/alignment exceptions, or comprehensive
hardware-specific device semantics. VU0 code writes are visible to the current interpreter fetch;
future compiled microcode engines will need explicit invalidation if
they cache decoded programs. `--hle-startup` is opt-in and only covers explicitly
recognized kernel calls; it does not implement BIOS, IOP, GS, SIF or general DMA.

## Confirmed real-ELF checkpoints

For the original US executable `SLUS_214.42`:

- ELF entry is **0x00100008**; main PT_LOAD begins at **0x00100000**.
- Initial entry CFG: **177 supported instructions** across **26 blocks**;
  LLVM IR parses/verifies and emits an x86-64 object.
- Full executable scan previously discovered **7,901 candidate functions**.
- At entry **0x001001c8** the guest invokes `SetupThread` (0x3c);
  at **0x001001e4** it invokes `SetupHeap` (0x3d).
- The native boot path calls into **0x0017f018**, **0x0017edc8**,
  **0x00174ca0**. The experimental host then successfully services two
  `CreateSema` (0x40) invocations, each with init/max count 1.
- The guest registers custom syscall **0x83 → 0x0017ee80** and
  **0x5a → 0x0017ee48** through `SetSyscall` (0x74).
- With seven native translations, the trace reaches **20 function
  dispatches** and stops at **0x0017eec4** invoking registered syscall
  **0x83**. The call arguments reference EE KSEG0 addresses
  **0x80000000..0x80080000**.

The native runner now has an experimental registered-syscall handoff:
when a SetSyscall-installed service is invoked, it dispatches the registered
guest handler as translated code, preserving the interrupted RA. On a return
through a dedicated synthetic return address it restores RA, clears EXL, and
resumes the instruction after the original syscall. Callback V0 and guest
memory writes are retained. A missing translated handler is surfaced as a
normal external dispatch PC so `--auto-expand` can compile it on replay.
Unexpected exceptions or departures from compiled code are *not* treated as
successful callback returns.

The user's submitted `first-boot.json` confirms the previous 20-dispatch
stop at `0x0017eec4`: COP0.Cause 0x20 (syscall), EPC `0x0017eec4`,
service `0x83`, with KSEG0 arguments `0x80000000` and `0x80080000`.
The installed handler is `0x0017ee80`, which is real game code. Its
execution and eventual return require a fresh game run; reaching a higher
PC or dispatch count must be verified rather than assumed.

The callback path is not a PS2 kernel exception implementation: nested
callback handoffs, accurate privilege/interrupt scheduling, and BIOS-specific
callback ABI details remain outside the currently verified scope.

## Why the callback keeps repeating

Disassembly of the user's US `SLUS_214.42` ELF confirms this isn't
necessarily a hardware-polling loop:

- Guest handler `0x0017EE80` loads 32-bit words from address A0 forward to
  address A1, comparing against A2. It returns the matching address in V0,
  or zero on an unsuccessful search.
- Startup function `0x0017EF08` invokes that handler through the registered
  `0x83` syscall, initially scanning physical EE RAM mirrored through
  `0x80000000..0x80080000` for the function addresses `0x0017EE80`
  and `0x0017EE48`. The caller subsequently adjusts the found addresses
  and repeats searches until a specific relative layout condition matches.
- The RAM-only runner previously stored successful SetSyscall(0x74)
  registrations solely in a native `installed_syscall_handlers` array.
  The equivalent `ps2-web-recomp` kernel HLE ALSO writes the registered
  handler to physical guest RAM at `0x00001800 + 4 * syscall_number`
  (KSEG0 `0x80001800 + 4 * syscall_number`), making the address visible
  to the game's kernel-memory search. The runner now mirrors successful
  registrations into the same guest-visible table, without inventing
  arbitrary kernel-memory contents. Other BIOS-initialized kernel data
  may still be required; a higher dispatch count is not proof of boot.

The runner's `--hle-startup --kseg-ram` path now mirrors the syscall
table without a snapshot. A synthetic ELF integration test verifies
uninitialized entries, two distinct handler slots, replacement of a
previously installed handler, and physical/KSEG0 alias reads.
For example, service 0x83 maps to physical 0x00001A0C, and service
0x5A maps to physical 0x00001968. Invalid registrations continue to
be rejected before altering the host dispatch table or guest RAM.
This behavior is modelled after `ps2-web-recomp/runtime/src/kernel.cpp`
(`kSyscallTable` and syscall 0x74). The existing guest-callback
integration test still verifies actual registered callback execution.

The runner also accepts `--kernel-ram=FILE --kseg-ram` to load an **optional,
exactly 524,288-byte raw snapshot** of physical EE RAM addresses
`0x00000000..0x0007FFFF` (equivalently KSEG0 `0x80000000..0x8007FFFF`)
*before* loading the game's ELF segments. This should be captured from an
actual running PS2/PCSX2 environment **at the game entry point**, before
these kernel patch searches execute. A PS2 BIOS ROM dump is not the same
as an initialized EE RAM snapshot. No proprietary BIOS image is bundled.

First test `--hle-startup --kseg-ram` **without** `--kernel-ram`.
Only if a later verified kernel-memory dependency persists, use
`--no-recompile --auto-expand=16 --kernel-ram=/path/to/ee-kernel-ram.bin`
with the existing translated library to test a captured snapshot.
A snapshot only supplies memory data; it does not create missing
kernel threads, interrupts, DMA or device behaviour. The table
mirroring is scoped to accepted SetSyscall registrations, not a
complete synthesized PS2 BIOS/kernel RAM image.

## Validation limits

CTest contains `game_runner_end_to_end` which creates small synthetic MIPS
ELFs, translates and links them to native host code and verifies that
dispatch and selected kernel calls resume correctly. The real-game observations
above were captured locally from the user's privately supplied ELF, not in CI.

The native host has bounded EE RAM, opt-in direct aliases, scratchpad,
VU0 apertures, a host MMIO bus, INTC and D_STAT register models, and bounded
normal/interleave SPR DMA channels 8/9 and chains for both channels,
but still no general TLB, BIOS, other DMA channels,
device scheduling, or full interrupt emulation. The syscall handling is deliberately
opt-in, experimental, and incomplete. Reaching the PCs above is not evidence
of full console initialization, correct kernel behaviour or graphical output.
