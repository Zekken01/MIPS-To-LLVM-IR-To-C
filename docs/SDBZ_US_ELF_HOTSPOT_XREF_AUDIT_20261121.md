# USA SDBZ ELF hotspot xref audit — 2026-11-21

## Evidence limitations

This audit uses the sixth matching PR #148 replay
(sdbz-graphics-pr148(5).zip), the prior verified USA ELF
analysis committed to the repository, and the native dispatcher.
The ZIP contains no SLUS_214.42 executable bytes. It is
therefore **not possible to assert exact original MIPS branch
instructions at every current hotspot from the ZIP alone**.

Proven retail USA ELF SHA-256:

1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16

## Previously decoded original ELF accessors

SDBZ_NATIVE_BOOT_PLAN.md documents:

| EE PC | Access / behaviour |
| --- | --- |
| 0x001751EC | Write 0x4 to EE INTC_STAT at 0x1000F000 (VBlank-start acknowledge) |
| 0x00175210 | Poll INTC_STAT & 0x4; fallback check on one-shot guest VSync flag |
| SetVSyncFlag | Register 0x01FFEEB0 as the flag and 0x01FFEEB8 as the GS CSR destination |

M2_4_SDBZ_COMMAND_SLOT_TIMEOUT.md documents a distinct original
game RAM/Timer pathway:

| EE PC | Access / behaviour |
| --- | --- |
| 0x001024D0 | Check 0x00502368 and 0x0050236C command-slot flags |
| 0x00102560 | Read active slot at 0x005030AC, XOR 1, load opposite slot flag |
| 0x001025A4..0x001025D4 | Poll outstanding opposite slot |
| 0x001025B4..0x001025C4 | Read EE Timer0 COUNT at 0x10000000 |
| 0x00102608..0x00102644 | Recovery helpers at 0x00173888 and 0x00173460 |
| 0x00102670 | Red-sprite error loop after exhausted recovery |

In the sixth replay, the sampled command-slot check at
dispatch 10411 saw selected slot 0 and polled slot 1,
flag0=1 and flag1=0. The pending-wait and red-loop
probes are both null: this snapshot is **not evidence
of entry to the error loop**.

## Which PC is actually current?

The sixth stopped report reaches the **configured
6,000,000 native dispatches**, not an unsupported opcode.
Final PC is **0x001094DC**, with observed incoming dispatch
boundaries through 0x001091E0, 0x00108220 and
0x00109230..0x001094C0. This is a different
location from the early VBlank check.

The dominant alternating dispatch pair is:

| PC | Native entry observations |
| --- | ---: |
| 0x00178DE8 | 1,828,364 |
| 0x001BD58C | 1,813,916 |

Combined these represent 60.7 percent of the 6m
native *dispatch entry* observations. Prior
SDBZ_IOP_HLE_REFERENCE_PARITY.md associates this
pair with ongoing EE/IOP RPC work. Native dispatch
edges are NOT direct JAL/JR instruction records and
are insufficient to identify exact memory operands.

Other hot pairs include 0x001BA584/0x001BAA10,
0x002CA088/0x002CA270 and
0x002F44D0/0x002F401C. Their original
instruction-level semantics are unverified.

Final INTc status is 0x1000 with mask 0x0804;
both one-shot registered VSync pointers are zero
and the GS has started 2,588 real fields.

## Read-only direct executable inspection

This PR adds tools/inspect_sdbz_elf_hotspots.py.
It reads the original PS2_GAME/SLUS_214.42
ELF directly on the user's Windows checkout,
enumerating candidate direct MIPS jump/branch callers
and locally resolvable source and destination memory
references at the original PCs.

It optionally uses existing ir/ee_functions.json
metadata for function owners and resolved JALR sites,
and existing first-boot.json for runtime hot PCs.
It does not modify or retranslate the ELF, guest IR,
cache manifest or native objects.

Run from a Windows Developer Command Prompt:

    cd /d C:\Dev\MIPS-To-LLVM-IR-To-C
    python tools\inspect_sdbz_elf_hotspots.py PS2_GAME\SLUS_214.42 ^
      --expected-sha256 1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16 ^
      --metadata build\ps2-game-hle\ir\ee_functions.json ^
      --runtime-json build\ps2-game-hle\first-boot.json ^
      --out build\ps2-game-hle\elf-hotspots.txt

The resulting elf-hotspots.txt is the evidence
necessary to name the exact MIPS instructions
accessing the currently hot addresses and
the full direct caller set. Candidate results
require CFG validation: MIPS ELF executable PT_LOAD
regions can contain embedded data, and local
constant propagation cannot resolve every indirect
load or call. Do not claim an absence of a writer
when the analyzer reports no *locally resolvable*
references.

The regression tests run on a tiny synthetic ELF
that proves original bytes, direct JAL/branch
callers, metadata-resolved JALR and MIPS
LUI/LW/SW memory references. No game assets
are committed.

## Next steps

Identify the exact caller and read/write operands
at 0x00178DE8, 0x001BD58C, 0x00108220 and
0x001094DC from the local executable report.
Cross-reference any identified SIF queue or
controller state word with original guest IOP RPC
and SIO2 registers before changing emulator logic.
The PCSX2 memory-card-warning screenshot is an
original-game reference, not verified native output;
do not inject an unsolicited button press.
