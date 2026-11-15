#include "ee_ram_interpreter.h"
#include "ee_memory.h"

#include <cstdint>
#include <cstdio>

namespace
{
bool check(bool ok, const char* message)
{
    if (!ok)
    {
        std::fprintf(stderr, "ee_ram_interpreter_test: %s\n", message);
    }
    return ok;
}

void write32(ee::ee_memory& memory, uint32_t address, uint32_t code)
{
    memory.write32(address, code);
}
}

int main()
{
    constexpr uint32_t k_ram_address = 0x80076000u;
    constexpr uint32_t k_table_address = 0x80076700u;
    constexpr uint32_t k_return_address = 0xfffffff0u;
    ee::ee_memory memory(2u * 1024u * 1024u);
    memory.set_kseg_ram_aliases(true);

    // A synthetic RAM routine using real MIPS branch and JR delay slots:
    // return a table value when a0 equals the table key, else return 0.
    // Delay slot on the BNE increments t0 in both paths; JR's delay slot
    // performs the final LW on the matching path.
    write32(memory, k_ram_address + 0x00u, 0x3c098007u); // lui t1, 0x8007
    write32(memory, k_ram_address + 0x04u, 0x25296700u); // addiu t1, t1, 0x6700
    write32(memory, k_ram_address + 0x08u, 0x8d2a0000u); // lw t2, 0(t1)
    write32(memory, k_ram_address + 0x0cu, 0x148a0003u); // bne a0,t2,+3
    write32(memory, k_ram_address + 0x10u, 0x25080001u); // addiu t0,t0,1
    write32(memory, k_ram_address + 0x14u, 0x03e00008u); // jr ra
    write32(memory, k_ram_address + 0x18u, 0x8d220004u); // lw v0,4(t1)
    write32(memory, k_ram_address + 0x1cu, 0x03e00008u); // jr ra
    write32(memory, k_ram_address + 0x20u, 0x0000102du); // daddu v0,zero,zero
    write32(memory, k_table_address, 0xfcu);
    write32(memory, k_table_address + 4u, 0x80076440u);

    ee::s_ee_cpu_state cpu{};
    cpu.m_gpr[4].m_lo = 0xfcu;
    cpu.m_gpr[31].m_lo = static_cast<uint64_t>(-16ll);
    const ee::s_ee_ram_execution found = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 50u
    );
    if (!check(found.m_result == ee::e_ee_ram_result::RETURNED,
               "matching key must return") ||
        !check(cpu.m_gpr[2].m_lo == 0xffffffff80076440ull,
               "JR delay slot must load and sign-extend the table pointer") ||
        !check(cpu.m_gpr[8].m_lo == 1u,
               "BNE delay slot must execute even when branch not taken") ||
        !check(found.m_instructions == 7u, "match instruction count") ||
        !check(cpu.m_pc == k_return_address, "return sentinel PC"))
    {
        return 1;
    }

    cpu = {};
    cpu.m_gpr[4].m_lo = 0xfeu;
    cpu.m_gpr[31].m_lo = k_return_address;
    const ee::s_ee_ram_execution not_found = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 50u
    );
    if (!check(not_found.m_result == ee::e_ee_ram_result::RETURNED,
               "missing key must return") ||
        !check(cpu.m_gpr[2].m_lo == 0u,
               "missing key must return zero") ||
        !check(cpu.m_gpr[8].m_lo == 1u,
               "BNE taken branch must still execute delay slot"))
    {
        return 1;
    }

    // Stop on unknown code; never silently replace it with a NOP.
    write32(memory, k_ram_address, 0x70000000u); // SPECIAL2, unsupported
    cpu = {};
    const ee::s_ee_ram_execution unknown = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 50u
    );
    if (!check(unknown.m_result == ee::e_ee_ram_result::UNSUPPORTED,
               "unknown instruction must fail closed") ||
        !check(unknown.m_pc == k_ram_address, "unsupported PC retained") ||
        !check(unknown.m_opcode == 0x70000000u, "unsupported opcode retained"))
    {
        return 1;
    }

    // Two-instruction infinite branch, including its delay slot; no hang.
    write32(memory, k_ram_address, 0x1000ffffu); // beq zero,zero,-1
    write32(memory, k_ram_address + 4u, 0x25080001u);
    cpu = {};
    const ee::s_ee_ram_execution bounded = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 20u
    );
    if (!check(bounded.m_result == ee::e_ee_ram_result::INSTRUCTION_BUDGET,
               "infinite branch must consume bounded budget") ||
        !check(bounded.m_instructions == 20u, "bounded instruction count") ||
        !check(cpu.m_gpr[8].m_lo == 10u, "loop delay slot executes each iteration"))
    {
        return 1;
    }

    // Unsupported control transfer in a branch delay slot fails closed.
    write32(memory, k_ram_address + 4u, 0x03e00008u);
    cpu = {};
    const ee::s_ee_ram_execution nested = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 20u
    );
    if (!check(nested.m_result == ee::e_ee_ram_result::BRANCH_IN_DELAY_SLOT,
               "branch in delay slot must not be executed as normal flow"))
    {
        return 1;
    }
    std::puts("ee_ram_interpreter_test: PASS");
    return 0;
}
