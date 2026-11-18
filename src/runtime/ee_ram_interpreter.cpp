#include "ee_ram_interpreter.h"
#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
namespace
{
struct s_ee_ram_step
{
    bool m_is_branch = false;
    bool m_taken = false;
    uint32_t m_target = 0;
};

uint64_t read_reg(const s_ee_cpu_state& cpu, uint32_t index)
{
    return index == 0u ? 0u : cpu.m_gpr[index].m_lo;
}

void write_word(s_ee_cpu_state& cpu, uint32_t index, uint32_t value)
{
    if (index != 0u)
    {
        // Scalar 32-bit MIPS operations sign-extend into a 64-bit GPR.
        cpu.m_gpr[index].m_lo = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int32_t>(value))
        );
    }
}

void write_dword(s_ee_cpu_state& cpu, uint32_t index, uint64_t value)
{
    if (index != 0u)
    {
        cpu.m_gpr[index].m_lo = value;
    }
}

uint32_t branch_target(uint32_t pc, uint16_t immediate)
{
    return pc + 4u +
        (static_cast<uint32_t>(
            static_cast<int32_t>(static_cast<int16_t>(immediate))
        ) << 2u);
}

bool checked_memory(const ee_memory& memory, uint32_t address, uint32_t width)
{
    return memory.contains(address, width) &&
        (width == 1u || (address & (width - 1u)) == 0u);
}

e_ee_ram_result step(
    s_ee_cpu_state& cpu,
    ee_memory& memory,
    uint32_t pc,
    uint32_t instruction,
    s_ee_ram_step& branch
)
{
    const uint32_t primary = instruction >> 26u;
    const uint32_t rs = (instruction >> 21u) & 31u;
    const uint32_t rt = (instruction >> 16u) & 31u;
    const uint32_t rd = (instruction >> 11u) & 31u;
    const uint32_t sa = (instruction >> 6u) & 31u;
    const uint16_t imm = static_cast<uint16_t>(instruction);
    const int64_t signed_imm = static_cast<int16_t>(imm);
    const uint64_t a = read_reg(cpu, rs);
    const uint64_t b = read_reg(cpu, rt);

    switch (primary)
    {
    case 0x00u:
        switch (instruction & 63u)
        {
        case 0x00u: // SLL, including NOP
            write_word(cpu, rd, static_cast<uint32_t>(b) << sa);
            return e_ee_ram_result::RETURNED;
        case 0x02u: // SRL
            write_word(cpu, rd, static_cast<uint32_t>(b) >> sa);
            return e_ee_ram_result::RETURNED;
        case 0x03u: // SRA
            write_word(cpu, rd, static_cast<uint32_t>(
                static_cast<int32_t>(b) >> sa));
            return e_ee_ram_result::RETURNED;
        case 0x08u: // JR
            branch.m_is_branch = true;
            branch.m_taken = true;
            branch.m_target = static_cast<uint32_t>(a);
            return e_ee_ram_result::RETURNED;
        case 0x09u: // JALR
            branch.m_is_branch = true;
            branch.m_taken = true;
            branch.m_target = static_cast<uint32_t>(a);
            write_dword(cpu, rd, pc + 8u);
            return e_ee_ram_result::RETURNED;
        case 0x21u: // ADDU
            write_word(cpu, rd,
                static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
            return e_ee_ram_result::RETURNED;
        case 0x23u: // SUBU
            write_word(cpu, rd,
                static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
            return e_ee_ram_result::RETURNED;
        case 0x24u: // AND
            write_dword(cpu, rd, a & b);
            return e_ee_ram_result::RETURNED;
        case 0x25u: // OR
            write_dword(cpu, rd, a | b);
            return e_ee_ram_result::RETURNED;
        case 0x26u: // XOR
            write_dword(cpu, rd, a ^ b);
            return e_ee_ram_result::RETURNED;
        case 0x27u: // NOR
            write_dword(cpu, rd, ~(a | b));
            return e_ee_ram_result::RETURNED;
        case 0x2au: // SLT
            write_dword(cpu, rd,
                static_cast<int64_t>(a) < static_cast<int64_t>(b) ? 1u : 0u);
            return e_ee_ram_result::RETURNED;
        case 0x2bu: // SLTU
            write_dword(cpu, rd, a < b ? 1u : 0u);
            return e_ee_ram_result::RETURNED;
        case 0x2du: // DADDU (MOVE)
            write_dword(cpu, rd, a + b);
            return e_ee_ram_result::RETURNED;
        case 0x2fu: // DSUBU
            write_dword(cpu, rd, a - b);
            return e_ee_ram_result::RETURNED;
        default:
            return e_ee_ram_result::UNSUPPORTED;
        }
    case 0x01u: // REGIMM
        if (rt != 0u && rt != 1u)
        {
            return e_ee_ram_result::UNSUPPORTED;
        }
        branch.m_is_branch = true;
        branch.m_taken = rt == 0u
            ? static_cast<int64_t>(a) < 0
            : static_cast<int64_t>(a) >= 0;
        branch.m_target = branch_target(pc, imm);
        return e_ee_ram_result::RETURNED;
    case 0x02u: // J
    case 0x03u: // JAL
        branch.m_is_branch = true;
        branch.m_taken = true;
        branch.m_target = ((pc + 4u) & 0xf0000000u) |
            ((instruction & 0x03ffffffu) << 2u);
        if (primary == 0x03u)
        {
            write_dword(cpu, 31u, pc + 8u);
        }
        return e_ee_ram_result::RETURNED;
    case 0x04u: // BEQ
    case 0x05u: // BNE
        branch.m_is_branch = true;
        branch.m_taken = primary == 0x04u ? a == b : a != b;
        branch.m_target = branch_target(pc, imm);
        return e_ee_ram_result::RETURNED;
    case 0x06u: // BLEZ
    case 0x07u: // BGTZ
        if (rt != 0u)
        {
            return e_ee_ram_result::UNSUPPORTED;
        }
        branch.m_is_branch = true;
        branch.m_taken = primary == 0x06u
            ? static_cast<int64_t>(a) <= 0
            : static_cast<int64_t>(a) > 0;
        branch.m_target = branch_target(pc, imm);
        return e_ee_ram_result::RETURNED;
    case 0x09u: // ADDIU
        write_word(cpu, rt, static_cast<uint32_t>(a) +
            static_cast<uint32_t>(signed_imm));
        return e_ee_ram_result::RETURNED;
    case 0x0au: // SLTI
        write_dword(cpu, rt, static_cast<int64_t>(a) < signed_imm ? 1u : 0u);
        return e_ee_ram_result::RETURNED;
    case 0x0bu: // SLTIU: sign-extend the immediate before unsigned comparison
        write_dword(cpu, rt,
            a < static_cast<uint64_t>(signed_imm) ? 1u : 0u);
        return e_ee_ram_result::RETURNED;
    case 0x0cu: // ANDI
        write_dword(cpu, rt, a & imm);
        return e_ee_ram_result::RETURNED;
    case 0x0du: // ORI
        write_dword(cpu, rt, a | imm);
        return e_ee_ram_result::RETURNED;
    case 0x0eu: // XORI
        write_dword(cpu, rt, a ^ imm);
        return e_ee_ram_result::RETURNED;
    case 0x0fu: // LUI
        if (rs != 0u)
        {
            return e_ee_ram_result::UNSUPPORTED;
        }
        write_word(cpu, rt, static_cast<uint32_t>(imm) << 16u);
        return e_ee_ram_result::RETURNED;
    case 0x19u: // DADDIU
        write_dword(cpu, rt, a + static_cast<uint64_t>(signed_imm));
        return e_ee_ram_result::RETURNED;
    case 0x20u: // LB
    case 0x21u: // LH
    case 0x23u: // LW
    case 0x24u: // LBU
    case 0x25u: // LHU
    case 0x27u: // LWU
    case 0x37u: // LD
    {
        const uint32_t address = static_cast<uint32_t>(
            a + static_cast<uint64_t>(signed_imm));
        const uint32_t width = primary == 0x20u || primary == 0x24u ? 1u :
            primary == 0x21u || primary == 0x25u ? 2u :
            primary == 0x37u ? 8u : 4u;
        if (!checked_memory(memory, address, width))
        {
            return e_ee_ram_result::UNMAPPED;
        }
        if (primary == 0x20u)
        {
            write_dword(cpu, rt, static_cast<uint64_t>(
                static_cast<int64_t>(static_cast<int8_t>(memory.read8(address)))));
        }
        else if (primary == 0x21u)
        {
            write_dword(cpu, rt, static_cast<uint64_t>(
                static_cast<int64_t>(static_cast<int16_t>(memory.read16(address)))));
        }
        else if (primary == 0x23u)
        {
            write_word(cpu, rt, memory.read32(address));
        }
        else if (primary == 0x24u)
        {
            write_dword(cpu, rt, memory.read8(address));
        }
        else if (primary == 0x25u)
        {
            write_dword(cpu, rt, memory.read16(address));
        }
        else if (primary == 0x27u)
        {
            write_dword(cpu, rt, memory.read32(address));
        }
        else
        {
            write_dword(cpu, rt, memory.read64(address));
        }
        return e_ee_ram_result::RETURNED;
    }
    case 0x28u: // SB
    case 0x29u: // SH
    case 0x2bu: // SW
    case 0x3fu: // SD
    {
        const uint32_t address = static_cast<uint32_t>(
            a + static_cast<uint64_t>(signed_imm));
        const uint32_t width = primary == 0x28u ? 1u :
            primary == 0x29u ? 2u : primary == 0x2bu ? 4u : 8u;
        if (!checked_memory(memory, address, width))
        {
            return e_ee_ram_result::UNMAPPED;
        }
        if (width == 1u)
        {
            memory.write8(address, static_cast<uint8_t>(b));
        }
        else if (width == 2u)
        {
            memory.write16(address, static_cast<uint16_t>(b));
        }
        else if (width == 4u)
        {
            memory.write32(address, static_cast<uint32_t>(b));
        }
        else
        {
            memory.write64(address, b);
        }
        return e_ee_ram_result::RETURNED;
    }
    default:
        return e_ee_ram_result::UNSUPPORTED;
    }
}

} // namespace

s_ee_ram_execution ee_execute_ram_callback(
    s_ee_cpu_state* cpu,
    ee_memory* memory,
    uint32_t entry_pc,
    uint32_t return_pc,
    uint32_t max_instructions,
    s_ee_runtime* runtime
)
{
    s_ee_ram_execution result{};
    result.m_pc = entry_pc;
    if (cpu == nullptr || memory == nullptr || (entry_pc & 3u) != 0u ||
        max_instructions == 0u)
    {
        result.m_result = e_ee_ram_result::UNMAPPED;
        return result;
    }

    uint32_t pc = entry_pc;
    bool delay_slot = false;
    bool pending_taken = false;
    uint32_t pending_target = 0;
    for (uint32_t n = 0; n < max_instructions; ++n)
    {
        if (pc == return_pc && !delay_slot)
        {
            result.m_result = e_ee_ram_result::RETURNED;
            result.m_pc = pc;
            cpu->m_pc = pc;
            return result;
        }
        result.m_pc = pc;
        cpu->m_pc = pc;
        if ((pc & 3u) != 0u || !memory->contains(pc, 4u))
        {
            result.m_result = e_ee_ram_result::UNMAPPED;
            return result;
        }
        const uint32_t instruction = memory->read32(pc);
        result.m_opcode = instruction;
        s_ee_ram_step branch{};
        const e_ee_ram_result action = step(
            *cpu, *memory, pc, instruction, branch
        );
        if (action != e_ee_ram_result::RETURNED)
        {
            result.m_result = action;
            return result;
        }
        if (delay_slot && branch.m_is_branch)
        {
            result.m_result = e_ee_ram_result::BRANCH_IN_DELAY_SLOT;
            return result;
        }
        ++result.m_instructions;
        // Interpreted RAM contributes committed instructions to the same
        // diagnostics as translated code; it cannot advance EE cycle time.
        if (runtime != nullptr)
        {
            ++runtime->m_guest_instruction_attempts;
            ++runtime->m_reference_instruction_slots;
            // Same bounded, optional observation domain as translated
            // execution. Unsupported/unmapped steps never emit an event.
            if (runtime->m_instruction_trace != nullptr &&
                pc >= runtime->m_instruction_trace_first_pc &&
                pc <= runtime->m_instruction_trace_last_pc)
            {
                if (runtime->m_instruction_trace_count <
                    runtime->m_instruction_trace_capacity)
                {
                    s_ee_instruction_trace_entry& entry =
                        runtime->m_instruction_trace[
                            runtime->m_instruction_trace_count++];
                    entry.m_attempt = runtime->m_guest_instruction_attempts;
                    entry.m_pc = pc;
                    entry.m_opcode_word = instruction;
                    entry.m_kind = 2u;
                    entry.m_reference_slot = runtime->m_reference_instruction_slots;
                    entry.m_hle_service = 0u;
                }
                else
                {
                    runtime->m_instruction_trace_overflow = 1u;
                }
            }
        }
        if (delay_slot)
        {
            delay_slot = false;
            pc = pending_taken ? pending_target : pc + 4u;
        }
        else if (branch.m_is_branch)
        {
            delay_slot = true;
            pending_taken = branch.m_taken;
            pending_target = branch.m_target;
            pc += 4u;
        }
        else
        {
            pc += 4u;
        }
    }
    result.m_pc = pc;
    cpu->m_pc = pc;
    result.m_result = (!delay_slot && pc == return_pc)
        ? e_ee_ram_result::RETURNED
        : e_ee_ram_result::INSTRUCTION_BUDGET;
    return result;
}

const char* ee_ram_result_name(e_ee_ram_result result)
{
    switch (result)
    {
    case e_ee_ram_result::RETURNED: return "returned";
    case e_ee_ram_result::UNMAPPED: return "unmapped or unaligned access";
    case e_ee_ram_result::UNSUPPORTED: return "unsupported R5900 instruction";
    case e_ee_ram_result::BRANCH_IN_DELAY_SLOT: return "branch in delay slot";
    case e_ee_ram_result::INSTRUCTION_BUDGET: return "instruction budget exhausted";
    default: return "unknown";
    }
}

} // namespace ee
