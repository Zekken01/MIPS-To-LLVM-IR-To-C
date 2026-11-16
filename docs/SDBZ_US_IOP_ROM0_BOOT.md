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

## Remaining work

- Implement ROM0-visible IOP hardware registers, exception/interrupt
  handling, timers and loadcore/system services as boot traces require.
- Evolve the standalone reset-vector execution into real IOP kernel
  scheduling and firmware module startup without fake guest threads.
- Derive proper EE/IOP execution synchronization from guest cycles.
- Confirm that firmware-originated SIF writes, not host startup,
  publish actual readiness flags.

The existing runner without a ROM0 image or manual IRXs is unchanged.
