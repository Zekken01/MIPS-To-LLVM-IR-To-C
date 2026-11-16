#include "iop_cpu.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace iop
{
namespace
{
int32_t sign16(uint32_t value)
{
    return static_cast<int16_t>(value & 0xffffu);
}

bool signed_sum(uint32_t left, uint32_t right, uint32_t* result)
{
    const int64_t sum = static_cast<int64_t>(static_cast<int32_t>(left)) +
                        static_cast<int64_t>(static_cast<int32_t>(right));
    const bool valid =
        sum >= std::numeric_limits<int32_t>::min() &&
        sum <= std::numeric_limits<int32_t>::max();
    if (valid && result != nullptr)
    {
        *result = static_cast<uint32_t>(sum);
    }
    return valid;
}

bool signed_difference(uint32_t left, uint32_t right, uint32_t* result)
{
    const int64_t difference =
        static_cast<int64_t>(static_cast<int32_t>(left)) -
        static_cast<int64_t>(static_cast<int32_t>(right));
    const bool valid =
        difference >= std::numeric_limits<int32_t>::min() &&
        difference <= std::numeric_limits<int32_t>::max();
    if (valid && result != nullptr)
    {
        *result = static_cast<uint32_t>(difference);
    }
    return valid;
}
} // namespace

c_cpu::c_cpu()
    : m_ram(k_ram_size, 0u)
{
    reset();
}

void c_cpu::reset(uint32_t entry)
{
    m_state = {};
    m_state.m_pc = entry;
    m_state.m_next_pc = entry + 4u;
    m_cycles = 0u;
    std::fill(m_ram.begin(), m_ram.end(), 0u);
}

bool c_cpu::install_rom0(const std::vector<uint8_t>& image)
{
    if (image.size() != k_rom0_size)
    {
        return false;
    }
    // Installing a new ROM never modifies the current IOP RAM or CPU.
    // It is persistent across IOP reset, just like hardware ROM0.
    m_rom0 = image;
    return true;
}

bool c_cpu::has_rom0() const
{
    return m_rom0.size() == k_rom0_size;
}

bool c_cpu::reset_rom0()
{
    if (!has_rom0())
    {
        return false;
    }
    reset(k_rom0_reset_pc);
    // PCSX2 R3000A reset: COP0 enabled, BEV and TS set, PRid 0x1f.
    m_state.m_cop0_status = 0x10900000u;
    m_state.m_cop0_prid = 0x0000001fu;
    return true;
}

const s_cpu_state& c_cpu::state() const
{
    return m_state;
}

s_cpu_state& c_cpu::state()
{
    return m_state;
}

uint64_t c_cpu::cycles() const
{
    return m_cycles;
}

bool c_cpu::read(uint32_t address, uint32_t width, uint32_t* value) const
{
    if (value == nullptr || (width != 1u && width != 2u && width != 4u))
    {
        return false;
    }

    // RAM is visible through the physical, KSEG0 and KSEG1 aliases.
    // Do not wrap unknown MMIO or unmapped physical addresses into RAM.
    const uint32_t region = address & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    const uint32_t physical = address & 0x1fffffffu;
    if ((address & (width - 1u)) != 0u)
    {
        return false;
    }

    const uint8_t* source = nullptr;
    uint32_t offset = 0u;
    if (physical <= k_ram_size - width)
    {
        source = m_ram.data();
        offset = physical;
    }
    else if (has_rom0() && physical >= k_rom0_physical &&
             static_cast<uint64_t>(physical - k_rom0_physical) + width <=
                 k_rom0_size)
    {
        source = m_rom0.data();
        offset = physical - k_rom0_physical;
    }
    else
    {
        return false;
    }

    uint32_t result = 0u;
    for (uint32_t index = 0u; index < width; ++index)
    {
        result |= static_cast<uint32_t>(
            source[offset + index]
        ) << (index * 8u);
    }
    *value = result;
    return true;
}

bool c_cpu::write(uint32_t address, uint32_t width, uint32_t value)
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }

    const uint32_t region = address & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    const uint32_t physical = address & 0x1fffffffu;
    if ((address & (width - 1u)) != 0u ||
        physical > k_ram_size - width)
    {
        return false;
    }

    for (uint32_t index = 0u; index < width; ++index)
    {
        m_ram[physical + index] =
            static_cast<uint8_t>(value >> (index * 8u));
    }
    return true;
}

bool c_cpu::read32(uint32_t address, uint32_t* value) const
{
    return read(address, 4u, value);
}

bool c_cpu::write32(uint32_t address, uint32_t value)
{
    return write(address, 4u, value);
}

void c_cpu::resume_syscall()
{
    m_state.m_pc = m_state.m_next_pc;
    m_state.m_next_pc += 4u;
}

e_step_result c_cpu::step(uint32_t* opcode)
{
    uint32_t word = 0u;
    if (!read32(m_state.m_pc, &word))
    {
        return e_step_result::memory_fault;
    }
    if (opcode != nullptr)
    {
        *opcode = word;
    }

    const uint32_t pc = m_state.m_pc;
    const uint32_t op = word >> 26;
    const uint32_t rs = (word >> 21) & 31u;
    const uint32_t rt = (word >> 16) & 31u;
    const uint32_t rd = (word >> 11) & 31u;
    const uint32_t shamt = (word >> 6) & 31u;
    const uint32_t funct = word & 63u;
    const uint32_t immediate = word & 0xffffu;
    const uint32_t left = m_state.m_gpr[rs];
    const uint32_t right = m_state.m_gpr[rt];
    const uint32_t next = m_state.m_next_pc;
    uint32_t next_after_delay = next + 4u;
    uint32_t value = 0u;

    const auto assign = [&](uint32_t index, uint32_t assigned)
    {
        if (index != 0u)
        {
            m_state.m_gpr[index] = assigned;
        }
    };
    const auto branch = [&](bool taken)
    {
        if (taken)
        {
            next_after_delay = next +
                (static_cast<uint32_t>(sign16(immediate)) << 2u);
        }
    };
    const auto read_word = [&](uint32_t address, uint32_t width,
                               uint32_t* destination)
    {
        return read(address, width, destination);
    };
    const uint32_t address = left +
        static_cast<uint32_t>(sign16(immediate));

    switch (op)
    {
    case 0u:
        switch (funct)
        {
        case 0x00u: assign(rd, right << shamt); break;
        case 0x02u: assign(rd, right >> shamt); break;
        case 0x03u:
            assign(rd, static_cast<uint32_t>(
                static_cast<int32_t>(right) >> shamt
            ));
            break;
        case 0x04u: assign(rd, right << (left & 31u)); break;
        case 0x06u: assign(rd, right >> (left & 31u)); break;
        case 0x07u:
            assign(rd, static_cast<uint32_t>(
                static_cast<int32_t>(right) >> (left & 31u)
            ));
            break;
        case 0x08u: next_after_delay = left; break;
        case 0x09u:
            assign(rd, pc + 8u);
            next_after_delay = left;
            break;
        case 0x0cu: return e_step_result::syscall;
        case 0x0du: return e_step_result::breakpoint;
        case 0x10u: assign(rd, m_state.m_hi); break;
        case 0x11u: m_state.m_hi = left; break;
        case 0x12u: assign(rd, m_state.m_lo); break;
        case 0x13u: m_state.m_lo = left; break;
        case 0x18u:
        {
            const int64_t product =
                static_cast<int64_t>(static_cast<int32_t>(left)) *
                static_cast<int64_t>(static_cast<int32_t>(right));
            const uint64_t bits = static_cast<uint64_t>(product);
            m_state.m_lo = static_cast<uint32_t>(bits);
            m_state.m_hi = static_cast<uint32_t>(bits >> 32u);
            break;
        }
        case 0x19u:
        {
            const uint64_t product =
                static_cast<uint64_t>(left) *
                static_cast<uint64_t>(right);
            m_state.m_lo = static_cast<uint32_t>(product);
            m_state.m_hi = static_cast<uint32_t>(product >> 32u);
            break;
        }
        case 0x1au:
        {
            const int32_t numerator = static_cast<int32_t>(left);
            const int32_t denominator = static_cast<int32_t>(right);
            if (denominator == 0)
            {
                m_state.m_lo = numerator < 0 ? 1u : 0xffffffffu;
                m_state.m_hi = left;
            }
            else if (numerator == std::numeric_limits<int32_t>::min() &&
                     denominator == -1)
            {
                m_state.m_lo = left;
                m_state.m_hi = 0u;
            }
            else
            {
                m_state.m_lo = static_cast<uint32_t>(
                    numerator / denominator
                );
                m_state.m_hi = static_cast<uint32_t>(
                    numerator % denominator
                );
            }
            break;
        }
        case 0x1bu:
            m_state.m_lo = right == 0u ? 0xffffffffu : left / right;
            m_state.m_hi = right == 0u ? left : left % right;
            break;
        case 0x20u:
            if (!signed_sum(left, right, &value))
            {
                return e_step_result::overflow;
            }
            assign(rd, value);
            break;
        case 0x21u: assign(rd, left + right); break;
        case 0x22u:
            if (!signed_difference(left, right, &value))
            {
                return e_step_result::overflow;
            }
            assign(rd, value);
            break;
        case 0x23u: assign(rd, left - right); break;
        case 0x24u: assign(rd, left & right); break;
        case 0x25u: assign(rd, left | right); break;
        case 0x26u: assign(rd, left ^ right); break;
        case 0x27u: assign(rd, ~(left | right)); break;
        case 0x2au:
            assign(rd, static_cast<int32_t>(left) <
                       static_cast<int32_t>(right) ? 1u : 0u);
            break;
        case 0x2bu: assign(rd, left < right ? 1u : 0u); break;
        default: return e_step_result::unsupported;
        }
        break;

    case 0x01u:
        switch (rt)
        {
        case 0x00u: branch(static_cast<int32_t>(left) < 0); break;
        case 0x01u: branch(static_cast<int32_t>(left) >= 0); break;
        case 0x10u:
            assign(31u, pc + 8u);
            branch(static_cast<int32_t>(left) < 0);
            break;
        case 0x11u:
            assign(31u, pc + 8u);
            branch(static_cast<int32_t>(left) >= 0);
            break;
        default: return e_step_result::unsupported;
        }
        break;

    case 0x02u:
    case 0x03u:
        if (op == 0x03u)
        {
            assign(31u, pc + 8u);
        }
        next_after_delay = (next & 0xf0000000u) |
                           ((word & 0x03ffffffu) << 2u);
        break;

    case 0x04u: branch(left == right); break;
    case 0x05u: branch(left != right); break;
    case 0x06u: branch(static_cast<int32_t>(left) <= 0); break;
    case 0x07u: branch(static_cast<int32_t>(left) > 0); break;

    case 0x08u:
        if (!signed_sum(left, static_cast<uint32_t>(sign16(immediate)), &value))
        {
            return e_step_result::overflow;
        }
        assign(rt, value);
        break;
    case 0x09u: assign(rt, address); break;
    case 0x0au:
        assign(rt, static_cast<int32_t>(left) < sign16(immediate) ? 1u : 0u);
        break;
    case 0x0bu:
        assign(rt, left < static_cast<uint32_t>(sign16(immediate)) ? 1u : 0u);
        break;
    case 0x0cu: assign(rt, left & immediate); break;
    case 0x0du: assign(rt, left | immediate); break;
    case 0x0eu: assign(rt, left ^ immediate); break;
    case 0x0fu: assign(rt, immediate << 16u); break;

    case 0x10u:
        if (rs == 0u)
        {
            switch (rd)
            {
            case 12u: assign(rt, m_state.m_cop0_status); break;
            case 13u: assign(rt, m_state.m_cop0_cause); break;
            case 14u: assign(rt, m_state.m_cop0_epc); break;
            case 15u: assign(rt, m_state.m_cop0_prid); break;
            default: return e_step_result::unsupported;
            }
        }
        else if (rs == 4u)
        {
            switch (rd)
            {
            case 12u: m_state.m_cop0_status = right; break;
            case 13u: m_state.m_cop0_cause = right; break;
            case 14u: m_state.m_cop0_epc = right; break;
            default: return e_step_result::unsupported;
            }
        }
        else if (word == 0x42000010u)
        {
            m_state.m_cop0_status =
                (m_state.m_cop0_status & ~0x0fu) |
                ((m_state.m_cop0_status >> 2u) & 0x0fu);
        }
        else
        {
            return e_step_result::unsupported;
        }
        break;

    case 0x20u:
    case 0x24u:
        if (!read_word(address, 1u, &value))
        {
            return e_step_result::memory_fault;
        }
        assign(rt, op == 0x20u
            ? static_cast<uint32_t>(static_cast<int32_t>(
                static_cast<int8_t>(value)
            ))
            : value);
        break;

    case 0x21u:
    case 0x25u:
        if (!read_word(address, 2u, &value))
        {
            return e_step_result::memory_fault;
        }
        assign(rt, op == 0x21u
            ? static_cast<uint32_t>(static_cast<int32_t>(
                static_cast<int16_t>(value)
            ))
            : value);
        break;

    case 0x22u:
    case 0x26u:
    {
        if (!read_word(address & ~3u, 4u, &value))
        {
            return e_step_result::memory_fault;
        }
        const uint32_t shift =
            ((op == 0x22u ? 3u - (address & 3u) : address & 3u) * 8u);
        if (op == 0x22u)
        {
            assign(rt, (right & (0x00ffffffu >> ((address & 3u) * 8u))) |
                       (value << shift));
        }
        else
        {
            const uint32_t preserved = shift == 0u ? 0u :
                (right & (0xffffffffu << (32u - shift)));
            assign(rt, preserved | (value >> shift));
        }
        break;
    }

    case 0x23u:
        if (!read_word(address, 4u, &value))
        {
            return e_step_result::memory_fault;
        }
        assign(rt, value);
        break;

    case 0x28u: if (!write(address, 1u, right)) return e_step_result::memory_fault; break;
    case 0x29u: if (!write(address, 2u, right)) return e_step_result::memory_fault; break;
    case 0x2bu: if (!write(address, 4u, right)) return e_step_result::memory_fault; break;

    case 0x2au:
    case 0x2eu:
    {
        if (!read_word(address & ~3u, 4u, &value))
        {
            return e_step_result::memory_fault;
        }
        const uint32_t shift =
            ((op == 0x2au ? 3u - (address & 3u) : address & 3u) * 8u);
        const uint32_t stored = op == 0x2au
            ? ((value & ~(0xffffffffu >> shift)) | (right >> shift))
            : ((value & ~(0xffffffffu << shift)) | (right << shift));
        if (!write(address & ~3u, 4u, stored))
        {
            return e_step_result::memory_fault;
        }
        break;
    }

    default:
        return e_step_result::unsupported;
    }

    m_state.m_pc = next;
    m_state.m_next_pc = next_after_delay;
    m_state.m_gpr[0] = 0u;
    ++m_cycles;
    return e_step_result::completed;
}

s_run_result c_cpu::run(uint32_t instruction_budget)
{
    s_run_result result{};
    result.m_result = e_step_result::budget_exhausted;
    for (uint32_t index = 0u; index < instruction_budget; ++index)
    {
        uint32_t opcode = 0u;
        const e_step_result next_result = step(&opcode);
        if (next_result != e_step_result::completed)
        {
            result.m_result = next_result;
            result.m_pc = m_state.m_pc;
            result.m_opcode = opcode;
            break;
        }
        ++result.m_instructions;
    }
    if (result.m_result == e_step_result::budget_exhausted)
    {
        result.m_pc = m_state.m_pc;
    }
    return result;
}
} // namespace iop
