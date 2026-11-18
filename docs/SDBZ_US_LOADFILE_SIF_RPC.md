# SDBZ US: guest SIF LOADFILE RPC binding and module-start transport

## Scope

The native runner now constructs a bounded IOP LOADFILE RPC service **only**
when an explicit guest IOP module is running. The actual game run
without `--iop-irx` still cannot perform IOP initialization, publish
SMFLAG, or reach the first RPC. No ELF candidate IRX is loaded
automatically at startup.

Once IOP guest code has actually invoked `sceSifInitCmd`, the EE
`sceSifSetDma` HLE can transfer an RPC packet to IOP command buffer
`0x0001E000`, and may transfer the parameter payload to the special
`0xDEADBEF0` SIF data destination.

The implemented SIFCMD RPC subset recognizes:

- `0x80000009` SIF RPC bind for **LOADFILE server `0x80000006`**.
- `0x8000000A` SIF RPC call to LOADFILE method 0.
- Method 0 requires a bounded send payload with the guest pathname
  at offset 8, and a real EE receive buffer for the eight-byte result.
- Nonzero IOP module argument length is deliberately rejected until
  argument propagation is supported. Unknown servers and methods
  also fail closed.

For a valid call, the guest-provided `cdrom0:` pathname passes into
`iop::c_disc_loader`. That loader opens the actual file under the
extracted disc root, resolves imports, creates the IRX's guest R3000
thread, and starts it. The RPC server returns the created module
identifier or a negative error, and queues a 48-byte SIF reply.

The reply is not injected into the EE until guest SIFCMD registered
an EE receive address via system command `0x80000002` **and** the EE
has armed SIF0 channel 5. The reply is written into the EE receive
buffer and raises the channel-5 D_STAT event. A second reply cannot
overwrite the first while channel 5 remains unacknowledged.

## Validated properties

- Synthetic R3000 guest code must execute `sceSifInitCmd` before
  the LOADFILE endpoint accepts traffic.
- Full SIF1 DMA descriptor validation runs before side effects.
- RPC bind and call receive the original guest identity fields.
- A distinct SIF1 data transfer supplies the original call payload.
- Invalid/unsupported packets, missing payload and unknown servers
  are rejected without partial IOP RAM changes or module execution.
- Successful IRX startup schedules an actual guest R3000 thread,
  which is executed by the regression harness.
- Missing disc modules return an error instead of success.
- No path synthesizes IOP SMFLAG readiness.

## Known limitations

This is a **bounded HLE LOADFILE subset**, not a BIOS, complete
SIF RPC implementation, or timing-accurate IOP runtime. A successful
module-start request is currently acknowledged once its guest thread
is queued, not after a fully modelled module-start callback. Full
argument passing, other LOADFILE methods, bidirectional SIFCMD
callbacks, SIF0 chain-tag details, correct hardware scheduling,
automatic IOP power-on and real firmware services are still required.

The most recent actual SDBZ US boot stopped at EE `0x001750B4`,
6,945 dispatches, waiting for an IOP-produced SMFLAG bit.
Do not claim a further game boot until a run with authentic IOP
startup demonstrates it.


## 2026-11-18 13:05: reboot metadata and LOADFILE/IOPHEAP service batch

The matched retail replay reached 6,960 dispatches, accepted SIF_CMD_RESET_CMD, and issued a 64-byte SIFRPC CALL to LOADFILE server 0x80000006, method 0xFF, with zero send bytes and four receive bytes. The method queries the actual LOADFILE version, which is now extracted from the selected on-disc IOPRP image's PsIIloadfile marker, not inferred from its filename or a default. A missing marker remains unsupported. Reset invalidates previous version metadata. The real result is delivered through armed EE SIF0.

IOPHEAP method 3 now copies actual mounted file bytes into a live, sufficiently large IOPHEAP allocation. Missing disc files return -2; invalid destinations, truncated paths, unknown methods and unbacked bytes fail closed. Regression covers the retail packet, version provenance, reset invalidation, file copies, failure replies and heap bounds. LOADFILE buffer method 6, module lookup method 9, nonzero module arguments, asynchronous completion and firmware IRX execution remain unimplemented. SetSyscall 0x12C remains unsupported.


## 2026-11-18 13:24: accepted RPC tracing and module scheduling

The matching local run has 100,000 dispatches and 3,871,691 guest instruction attempts (the configured dispatch cap), with four new reachable EE functions and no unsupported RPC stop. The trace repeats guest PCs 0x0018E0A0 and 0x001938E0 between successive SIF1 request/reply cycles. The static disc catalog still shows runtime_load_count=0 but is **not wired to runtime LOADFILE**, so that number is not evidence of zero actual loads.

The HLE bridge now retains a side-effect-free diagnostic snapshot of **committed** RPC operations (CID, server, method, client, sizes, return word, path, LOADFILE backend/link status, module and thread IDs). The runner logs the actual service results, collapsing identical requests unless --trace was requested. After a successful real IRX LOADFILE startup it schedules a bounded guest R3000 slice instead of leaving its thread ready forever when EE WaitSema never blocks; non-idle/non-budget stop reasons remain explicit hard failures. No fabricated module starts, completed RPCs or IOP readiness are introduced. The integrated regression asserts real version RPC and successful/missing LOADFILE diagnostics, and verifies the real IRX entrypoint executes on the guest CPU.


A real R3000 module may require more than one bounded IOP quantum. The native HLE runner now gives the IOP scheduler a bounded turn after each accepted SIFRPC transport, not only on the initial LOADFILE success; ordinary guest threads resume across calls. Invalid/unhandled guest instructions and syscalls still stop with their actual PC/opcode. A regression fixture executes a NOP in the first limited scheduler slice and reaches its breakpoint only on the later turn. No invented successful module load is implied.

The guest transport scheduler only receives bounded turns when `c_kernel::has_guest_work()` finds a ready R3000 thread or a time-blocked guest thread. No modules, dormant threads, hard-faulted threads, and untimed waits cannot advance IOP cycles from a spurious HLE SIFRPC poll. Regression checks the transition empty → ready → ready after first quantum → faulted after breakpoint.
