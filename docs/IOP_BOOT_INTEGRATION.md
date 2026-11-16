# IOP boot integration for SDBZ (US)

Status: staged integration; **the IOP is not yet booted by `ee_game_runner`**.

## Reproduced stop state (2026-11-16)

The native EE runner reaches 6,945 dispatches and stops at guest syscall
`0x001750B4`, which executes `sceSifGetReg(4)` in a tight three-dispatch
loop. `4` denotes IOP-owned SMFLAG. The value remains zero because there
is no IOP producer. With `--sif-poll-limit=256`, the runner now stops with
`EE_EXIT_UNSUPPORTED` and reports the actual stalled syscall PC instead of
silently running to the 100,000-dispatch budget.

**Do not simply change the SMFLAG reset value in `ee_sif.h`.**
That would bypass this synchronization point without implementing the
subsequent IOP-side command buffer, DMA and RPC services.

## Reference implementation

`twist84/ps2-web-recomp`, `SDBZ` branch:

- `runtime/src/iop/cpu.cpp`: MIPS I/R3000 interpreter.
- `runtime/src/iop/kernel.cpp`: IOP thread/event scheduler, IRX module
  loading, and `iop::run(ee_cycles)` (EE:IOP clock conversion of 8:1).
- `runtime/src/iop/sif.cpp`: IOP command handling, DMA and RPC delivery,
  Arika command dispatch; `runtime/src/iop/arika_dvd.cpp` handles game assets.
- `runtime/src/iop/sif.cpp::reset()` explicitly initializes
  `g_smflag = 0x60000`. **This is an HLE bootstrap value**, not evidence
  that a genuine R3000 thread produced the flag. Do not rebrand copying it
  into our EE runner as full IOP emulation.

## Integration milestones

1. **Standalone CPU execution (current PR).** `iop::c_cpu` owns 2 MiB of
   isolated IOP RAM and executes a bounded MIPS I subset with native MIPS
   branch/delay behavior, HI/LO, unaligned loads/stores, and explicit traps.
   Unsupported MMIO, kernel syscalls and instructions fail closed. This does
   not create SIF state or execute any module in the game runner.
2. **IOP kernel and IRX loader.** Reuse/adapt the reference IRX loader and
   scheduler. Implement module allocation/import resolution, threads,
   interrupts and syscall/HLE boundaries against this CPU and RAM. Use real
   game module bytes supplied at runtime, not committed copyrighted binaries.
3. **Cycle/event integration.** Advance IOP execution from an explicit,
   deterministic EE-cycle source. Do not derive hardware cycles from native
   dispatch count: one dispatch can execute widely different numbers of
   guest instructions. Make thread/scheduler state separately reportable.
4. **SIF producer connection.** Wire IOP-owned SMCOM/SMFLAG writes into the
   *existing* `c_ee_sif::publish_iop_subaddr()` and
   `publish_iop_smflag()` hooks, and deliver EE-owned MSCOM/MSFLAG
   notifications in the opposite direction. Ensure writes are ordered
   against EE SIF register syscalls and MMIO reads.
5. **SIF DMA/commands/RPC.** Implement real packet transfers and the
   required module services. The completed SMFLAG handshake is only the
   first dependency; the game will need SIFCMD and SIFRPC services.
6. **Validation.** Keep the synthetic CPU and SIF tests, capture an actual
   PCSX2 SIF trace to compare command order and data, and verify SDBZ can
   progress past `0x001750B4` without pre-setting any IOP-owned ready bit.

## Separation of responsibilities

The `ee_recomp` LLVM generator, EE native function metadata and
`verify_ir.py` are unchanged by the first milestone. The new IOP core is
a separate native CMake target and uses no game-specific executable hashes
or function addresses. No unsupported opcode is treated as NOP, and no
successful IOP boot is reported until the IOP kernel actually runs.
