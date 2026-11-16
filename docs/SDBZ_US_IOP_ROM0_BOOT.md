# SDBZ US: authentic IOP ROM0 reset-vector bootstrap (experimental)

## Why this exists

The SDBZ US EE has consistently stalled at `0x001750B4` polling
IOP-owned SMFLAG `0x00020000` while no IOP producer is running.
The game ELF's seven IRX path references **cannot** initialize a
PS2 IOP by themselves: its resident kernel is bootstrapped from
the PS2 BIOS. Previous IRX/SIF bridges have no firmware source.

PCSX2's IOP reset starts at `0xBFC00000` with COP0 Status
`0x10900000` and PRid `0x1F`. Its ROM0 is 4 MiB mapped at physical
`0x1FC00000` and KSEG0/KSEG1 aliases.

References:

- https://sources.debian.org/src/pcsx2/1.5.0~gfc1d9aef0%2Bdfsg-1/pcsx2/R3000A.cpp
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Memory.cpp

## Implementation

- The R3000 CPU can map a caller-provided 4 MiB ROM0 image **read-only**
  at physical `0x1FC00000`, `0x9FC00000`, and `0xBFC00000`.
- The IOP ROM reset starts at `0xBFC00000`, resets IOP RAM and registers,
  and sets the verified COP0 Status and PRid.
- The IOP kernel executes ROM instructions directly in a bounded
  instruction budget. No invented module thread, HLE call result,
  readiness bit, or synthetic IOP init is involved.
- Unsupported MMIO, syscalls, and instructions remain hard stops
  with guest PC and opcode. Read-only ROM writes are rejected.
- ROM0 bootstrap and the existing `--iop-irx` experimental module
  bootstrap are deliberately mutually exclusive.

## Running with a locally obtained BIOS image

No BIOS data is bundled with this project. Use an authentic primary
4 MiB ROM0 dump obtained from hardware you are entitled to use.
Rebuild the native runner after pulling this change, but the existing
translated EE guest DLL does not need recompilation.

To use the unchanged `tools/run_boot_session.py` and `run_game.py`
with `--no-recompile`, set the host-only environment variable in
your Windows CMD shell before running your existing boot command:

```bat
set "SDBZ_IOP_ROM0=C:\path\to\your\bios-rom0.bin"
python tools/run_boot_session.py PS2_GAME\SLUS_214.42 --work-dir=build\sdbz-thread-pr38 --report=build\sdbz-thread-pr38\first-boot.json --no-recompile --hle-startup --kseg-ram --sif-poll-limit=256 --trace
```

Alternatively, the `ee_game_runner` executable supports
`--iop-rom0=FILE` directly. Do not pass that option to `run_game.py`,
which deliberately stays unchanged to preserve translation-cache
provenance.

Expected diagnostic before full hardware support:

```text
IOP ROM0 reset: pc=0xbfc00000 cp0_status=0x10900000 cp0_prid=0x0000001f ...
IOP guest stopped: reason=... thread=0 pc=0x... opcode=0x...
```

This may stop early at the first BIOS MMIO register access or other
unimplemented instruction. That is valuable evidence for implementing
the next device and a much more legitimate milestone than forcing
SMFLAG `0x00020000` to ready.

## Real-game ROM0 scheduling and reproducible SIF readiness (PR #103)

The game runner now selects a 65,536-instruction *ROM0* quantum by
default (versus 256 for the explicit IRX/HLE mode). The EE\'s 256-read
SMFLAG stall guard previously permitted only 65,536 IOP instructions total,
whereas the standalone OpenPS2BIOS hardware integration grants 10 million
instructions before asserting guest firmware readiness. This is a bounded
deterministic rendezvous, **not** EE/IOP cycle-accurate scheduling.

The firmware guest runs its own `sifcmd` / `sifman` logic. Unlike explicit
 IRX/HLE mode, authentic ROM0 mode does **not** install their compatibility
stubs into IOP RAM, does not create compatibility LOADFILE RPC responses, and
does not synthesize SIF readiness. On a failed poll guard it logs the actual
IOP PC, executed instruction count, receive buffer, DMA10 CHCR and hardware
SIF0/SIF1 byte counters to distinguish CPU progress from transport stalls.

For the unchanged Python translation driver and `--no-recompile` cache, set
host-only environment variables in CMD:

```bat
set "SDBZ_IOP_ROM0=C:\\path\\to\\your\\bios-rom0.bin"
rem Optional: override the default ROM0 guest instruction quantum (1..65536)
set "SDBZ_IOP_ROM0_QUANTUM=65536"
run_sdbz_native.bat
```

The direct `ee_game_runner` command also accepts
`--iop-quantum=N`; that explicit argument takes precedence over the
environment variable. The existing `run_game.py` script intentionally
does not forward this option for ROM0; use the environment variable instead.

With ROM0 enabled, guest EE `sceSifSetDma` (syscall `0x77`)
now accepts bounded, prevalidated 16-byte descriptor lists. Its host
kernel boundary builds a real EE DMA6 source chain with the IOP DMA10
destination tags and quadword-padded snapshots of guest EE source bytes.
The existing DMA6 registers, FIFO and genuine IOP DMA10 receiver move
the bytes; there is no HLE command response or injected readiness.

Temporary staging borrows EE RAM at `0x01c00000` and restores its original
bytes before guest execution resumes. Each request is bounded to 64
descriptors and 64 KiB of staged packet data; only completed DMA IDs
return `SifDmaStat=-1`. This is synchronous host kernel marshalling,
**not** EE/IOP cycle-accurate scheduling.

The official OpenPS2BIOS v0.6 integration test now sends its genuine
LOADFILE `GET_VERSION` RPC_CALL using this same syscall adapter and
requires a firmware-generated SIF0 result. Smoke tests verify a 20-byte
request with zero-padded QWC, two descriptor transfers, refusal without an
IOP DMA10 receiver, invalid-attribute rejection, interrupt completion and
EE RAM restoration.

**This is still not a full SDBZ boot:** guest EE DMA5 interrupt handler
delivery, complete EE exception and interrupt scheduling, and disc-backed
LOADFILE module requests remain incomplete. Never replace guest firmware
readiness or replies with host-injected flags or synthetic packets.

## Remaining work

- Implement ROM0-visible IOP hardware registers, exception/interrupt
  handling, timers and loadcore/system services as boot traces require.
- Evolve the standalone reset-vector execution into real IOP kernel
  scheduling and firmware module startup without fake guest threads.
- Derive proper EE/IOP execution synchronization from guest cycles.
- Confirm that firmware-originated SIF writes, not host startup,
  publish actual readiness flags.

The existing runner without a ROM0 image or manual IRXs is unchanged.
