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
