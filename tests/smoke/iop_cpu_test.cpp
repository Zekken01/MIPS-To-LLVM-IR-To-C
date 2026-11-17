#include "iop_cpu.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP R3000 execution: %s\n", message);
    return 1;
}

bool write_program(iop::c_cpu& cpu, uint32_t address,
                   const uint32_t* instructions, uint32_t count)
{
    bool valid = true;
    for (uint32_t index = 0u; index < count; ++index)
    {
        valid = valid && cpu.write32(
            address + index * 4u, instructions[index]
        );
    }
    return valid;
}
} // namespace

int main()
{
    iop::c_cpu cpu;

    // Branch delay-slot writes must execute, while the instruction
    // skipped by the taken BEQ must not execute. The guest accesses
    // RAM through KSEG0 and the physical mirror interchangeably.
    cpu.reset(0x80000000u);
    const uint32_t branch_program[] =
    {
        0x24080002u, // addiu t0,zero,2
        0x11080002u, // beq t0,t0,+2: target 0x80000010
        0x25080003u, // delay slot: addiu t0,t0,3
        0x25080063u, // skipped
        0xac080100u, // sw t0,0x100(zero)
        0x8c090100u, // lw t1,0x100(zero)
        0x0000000cu  // syscall: fail closed
    };
    if (!write_program(cpu, 0u, branch_program, 7u))
    {
        return fail("cannot populate IOP RAM");
    }

    iop::s_run_result result = cpu.run(20u);
    uint32_t word = 0u;
    if (result.m_result != iop::e_step_result::syscall ||
        result.m_instructions != 5u ||
        result.m_pc != 0x80000018u ||
        cpu.state().m_gpr[8] != 5u ||
        cpu.state().m_gpr[9] != 5u ||
        cpu.cycles() != 5u ||
        !cpu.read32(0xa0000100u, &word) || word != 5u)
    {
        return fail("branch delay slot, store/load, RAM mirror or syscall");
    }
    cpu.resume_syscall();
    if (cpu.state().m_pc != 0x8000001cu)
    {
        return fail("guest syscall explicitly resumed at wrong PC");
    }

    // A JAL publishes the correct return address *before* its delay
    // slot; JR also executes its delay slot before resuming at the caller.
    cpu.reset(0u);
    const uint32_t call_program[] =
    {
        0x0c000008u, // jal 0x20, link 0x08
        0x24090002u, // jal delay: t1=2
        0x24080008u, // after return: t0=8
        0x0000000cu  // syscall
    };
    const uint32_t callee[] =
    {
        0x24080005u, // t0=5
        0x03e00008u, // jr ra
        0x25080001u  // delay: t0=6
    };
    if (!write_program(cpu, 0u, call_program, 4u) ||
        !write_program(cpu, 0x20u, callee, 3u))
    {
        return fail("cannot populate IOP call fixture");
    }
    result = cpu.run(20u);
    if (result.m_result != iop::e_step_result::syscall ||
        result.m_pc != 0x0000000cu ||
        result.m_instructions != 6u ||
        cpu.state().m_gpr[31] != 8u ||
        cpu.state().m_gpr[8] != 8u ||
        cpu.state().m_gpr[9] != 2u)
    {
        return fail("JAL/JR branch delay, return address or instructions");
    }

    // MIPS I signed HI/LO and unsigned memory paths.
    cpu.reset(0u);
    const uint32_t arithmetic[] =
    {
        0x2408fff9u, // t0=-7
        0x24090003u, // t1=3
        0x01090018u, // mult t0,t1 -> -21
        0x00005012u, // mflo t2
        0x0109001au, // div t0,t1 -> -2, remainder -1
        0x00005812u, // mflo t3
        0x00006010u, // mfhi t4
        0x0000000du  // break
    };
    if (!write_program(cpu, 0u, arithmetic, 8u))
    {
        return fail("cannot populate arithmetic fixture");
    }
    result = cpu.run(20u);
    if (result.m_result != iop::e_step_result::breakpoint ||
        result.m_instructions != 7u ||
        cpu.state().m_gpr[10] != static_cast<uint32_t>(-21) ||
        cpu.state().m_gpr[11] != static_cast<uint32_t>(-2) ||
        cpu.state().m_gpr[12] != static_cast<uint32_t>(-1))
    {
        return fail("signed MULT/DIV and HI/LO semantics");
    }

    // MIPS trap instructions continue normally when their predicate is
    // false. OpenPS2BIOS uses TEQ as an assertion on a successful path.
    cpu.reset(0u);
    const uint32_t trap_not_taken[] =
    {
        0x240c0001u, // addiu t4,zero,1
        0x018001f4u, // teq t4,zero,7: false, so continue
        0x240d0055u, // addiu t5,zero,0x55
        0x0000000du  // break
    };
    if (!write_program(cpu, 0u, trap_not_taken, 4u))
    {
        return fail("cannot populate non-trapping trap fixture");
    }
    result = cpu.run(10u);
    if (result.m_result != iop::e_step_result::breakpoint ||
        result.m_instructions != 3u || cpu.state().m_gpr[13] != 0x55u)
    {
        return fail("false MIPS trap predicate did not continue");
    }

    // Unaligned MIPS I paired LWL/LWR loads reconstruct a 32-bit word.
    cpu.reset(0u);
    const uint32_t unaligned[] =
    {
        0x24080100u, // addiu t0,zero,0x100
        0x89090004u, // lwl t1,4(t0)
        0x99090001u, // lwr t1,1(t0)
        0x0000000du  // break
    };
    if (!write_program(cpu, 0u, unaligned, 4u) ||
        !cpu.write32(0x100u, 0x44332211u) ||
        !cpu.write32(0x104u, 0x88776655u))
    {
        return fail("cannot populate unaligned fixture");
    }
    result = cpu.run(10u);
    if (result.m_result != iop::e_step_result::breakpoint ||
        cpu.state().m_gpr[9] != 0x55443322u)
    {
        return fail("little-endian LWL/LWR semantics");
    }

    // Unknown MMIO must not alias low physical RAM. Unsupported opcodes
    // and bad alignment must stop without advancing or fabricating state.
    cpu.reset(0u);
    const uint32_t invalid[] =
    {
        0x3c081f80u, // lui t0,0x1f80 (IOP MMIO)
        0x8d091000u, // lw t1,0x1000(t0)
    };
    if (!write_program(cpu, 0u, invalid, 2u))
    {
        return fail("cannot populate invalid-memory fixture");
    }
    result = cpu.run(5u);
    if (result.m_result != iop::e_step_result::memory_fault ||
        result.m_instructions != 1u || result.m_pc != 4u)
    {
        return fail("unmapped IOP MMIO did not fail closed");
    }
    const iop::s_memory_fault& fault = cpu.last_memory_fault();
    if (!fault.m_valid || fault.m_address != 0x1f801000u ||
        fault.m_width != 4u || fault.m_write)
    {
        return fail("R3000 MMIO fault address/width/access was lost");
    }

    cpu.reset(0u);
    if (!cpu.write32(0u, 0xffffffffu) ||
        cpu.run(10u).m_result != iop::e_step_result::unsupported ||
        cpu.state().m_pc != 0u)
    {
        return fail("unknown IOP instruction did not stop at faulting PC");
    }

    cpu.reset(0u);
    const uint32_t budget_program[] =
    {
        0x1000ffffu, // beq zero,zero,-1
        0x00000000u  // nop delay
    };
    if (!write_program(cpu, 0u, budget_program, 2u))
    {
        return fail("cannot populate bounded-loop fixture");
    }
    result = cpu.run(33u);
    if (result.m_result != iop::e_step_result::budget_exhausted ||
        result.m_instructions != 33u || cpu.cycles() != 33u)
    {
        return fail("bounded IOP execution did not return to host");
    }

    std::printf("IOP R3000 execution: PASS\n");
    return 0;
}
