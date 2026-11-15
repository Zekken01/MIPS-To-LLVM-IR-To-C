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
partially or pretending to complete. It does **not** implement
chain/interleave DMA, GIF/VIF/IPU transfers, MFIFO, channels 0..7, or
cycle-based DMA completion. `fromSPR` channel 8 also supports
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
RAM or raise a false completion interrupt. toSPR chain mode, MFIFO,
DMA interleave, and timed completion are still unsupported.
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

This still does **not** implement toSPR chains, DMA interleave,
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
- Our RAM-only host initializes that region to zero by default, not to the
  live EE kernel state normally established by the PS2 BIOS. Thus the
  translated search can behave correctly while the expected addresses never
  appear. Increasing dispatch or wall-clock budgets alone cannot supply
  the missing kernel memory.

The runner now accepts `--kernel-ram=FILE --kseg-ram` to load an **optional,
exactly 524,288-byte raw snapshot** of physical EE RAM addresses
`0x00000000..0x0007FFFF` (equivalently KSEG0 `0x80000000..0x8007FFFF`)
*before* loading the game's ELF segments. This should be captured from an
actual running PS2/PCSX2 environment **at the game entry point**, before
these kernel patch searches execute. A PS2 BIOS ROM dump is not the same
as an initialized EE RAM snapshot. No proprietary BIOS image is bundled.

Use `--no-recompile --auto-expand=16 --kernel-ram=/path/to/ee-kernel-ram.bin`
with the existing translated library to test a captured snapshot. This
only supplies memory data; it does not create missing kernel threads,
interrupts, DMA, or device behaviour. Without a real capture, leave the
option unset and treat unsuccessful searches as a missing-kernel-state
boundary rather than falsifying their return values.

## Validation limits

CTest contains `game_runner_end_to_end` which creates small synthetic MIPS
ELFs, translates and links them to native host code and verifies that
dispatch and selected kernel calls resume correctly. The real-game observations
above were captured locally from the user's privately supplied ELF, not in CI.

The native host has bounded EE RAM, opt-in direct aliases, scratchpad,
VU0 apertures, a host MMIO bus, INTC and D_STAT register models, and bounded
normal-mode SPR DMA channels 8/9 and channel-8 chains, but still no general TLB, BIOS, other DMA channels,
device scheduling, or full interrupt emulation. The syscall handling is deliberately
opt-in, experimental, and incomplete. Reaching the PCs above is not evidence
of full console initialization, correct kernel behaviour or graphical output.
