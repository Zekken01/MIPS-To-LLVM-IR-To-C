// Derived from ps2-web-recomp:SDBZ runtime/src/vu_micro.cpp
// (Samir Nuri, 2026, MIT). See ps2_webgpu/gs/LICENSE.upstream.
// This first batch intentionally rejects unported instructions and timing
// dependencies instead of silently treating them as NOPs.
#include "ee_vu1.h"

#include <algorithm>
#include <cstring>

namespace ee
{
namespace
{
uint32_t read32(const uint8_t* bytes)
{
    uint32_t result = 0u;
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}

uint64_t read64(const uint8_t* bytes)
{
    uint64_t result = 0u;
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}

float to_float(uint32_t bits)
{
    // Reference VU input normalisation: flush denormals, saturate INF/NaN.
    const uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0x7f800000u)
    {
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

uint32_t to_bits(float value)
{
    uint32_t result = 0u;
    std::memcpy(&result, &value, sizeof(result));
    const uint32_t exponent = result & 0x7f800000u;
    if (exponent == 0u)
    {
        result &= 0x80000000u;
    }
    else if (exponent == 0x7f800000u)
    {
        result = (result & 0x80000000u) | 0x7f7fffffu;
    }
    return result;
}

void store_vi(s_ee_vu1_state& state, uint32_t index, uint32_t value)
{
    if (index != 0u)
    {
        state.m_vi[index] = static_cast<uint16_t>(value);
    }
}

uint32_t qword_offset(uint32_t address)
{
    return (address & 0x3ffu) * 16u;
}

// XGKICK walks actual GIFtags from VU1 RAM until an EOP tag. A missing EOP
// or oversized chain is an error, never a fabricated empty PATH1 transfer.
bool xgkick(const std::array<uint8_t, 16384>& data,
            uint32_t address, std::vector<uint8_t>& output)
{
    std::vector<uint8_t> packet;
    uint32_t qword = address & 0x3ffu;
    bool finished = false;
    for (uint32_t count = 0u; count < 1024u && !finished;)
    {
        const uint32_t at = qword_offset(qword++);
        const uint64_t tag = read64(data.data() + at);
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7fffu);
        const uint32_t flg = static_cast<uint32_t>((tag >> 58u) & 3u);
        uint32_t nreg = static_cast<uint32_t>((tag >> 60u) & 15u);
        if (nreg == 0u)
        {
            nreg = 16u;
        }
        const uint32_t body = flg == 0u ? nloop * nreg :
            flg == 1u ? (nloop * nreg + 1u) / 2u : nloop;
        if (body + 1u > 1024u - count)
        {
            return false;
        }
        packet.insert(packet.end(), data.data() + at, data.data() + at + 16u);
        ++count;
        for (uint32_t i = 0u; i < body; ++i)
        {
            const uint32_t body_at = qword_offset(qword++);
            packet.insert(packet.end(),
                          data.data() + body_at, data.data() + body_at + 16u);
            ++count;
        }
        finished = (tag & 0x8000u) != 0u;
    }
    if (!finished || output.size() + packet.size() > 0x100000u)
    {
        return false;
    }
    output.insert(output.end(), packet.begin(), packet.end());
    return true;
}

struct s_upper_write
{
    std::array<uint32_t, 4> m_values{};
    uint32_t m_target = 0u;
    uint32_t m_mask = 0u;
    bool m_acc = false;
};

bool upper(const s_ee_vu1_state& state, uint32_t hi,
           s_upper_write& write)
{
    const uint32_t funct = hi & 63u;
    const uint32_t index = (((hi >> 6u) & 31u) << 2u) | (funct & 3u);
    const uint32_t mask = (hi >> 21u) & 15u;
    const uint32_t fs = (hi >> 11u) & 31u;
    const uint32_t ft = (hi >> 16u) & 31u;
    const uint32_t fd = (hi >> 6u) & 31u;
    write.m_mask = mask;
    write.m_target = fd;

    uint32_t operation = funct;
    bool broadcast = funct < 0x1cu;
    uint32_t operand = 0u;
    if (funct >= 0x3cu)
    {
        // Canonical VU NOP (special index 0x2f); reject unknown specials.
        if (index == 0x2fu)
        {
            write.m_mask = 0u;
            return true;
        }
        // ACC arithmetic, conversion, flag operations, clip and special
        // pipelines are implemented in a later batch. Never guess effects.
        return false;
    }
    // Validate even if the destination lane mask is zero. Unsupported
    // opcode families must never become accidental successful NOPs.
    const bool supported =
        operation <= 0x07u ||
        (operation >= 0x10u && operation <= 0x1bu) ||
        operation == 0x1eu || operation == 0x22u ||
        operation == 0x26u || operation == 0x28u ||
        operation == 0x2au || operation == 0x2bu ||
        operation == 0x2cu || operation == 0x2fu;
    if (!supported)
    {
        return false;
    }

    // Source selectors: 0..27 use broadcast lanes, 28..2f use Q/I or
    // vector operations. Q-dependent ops need the real Q timing pipeline.
    if ((operation >= 0x1cu && operation <= 0x1fu) ||
        (operation >= 0x20u && operation <= 0x27u))
    {
        if (operation == 0x1du || operation == 0x1eu ||
            operation == 0x1fu || (operation & 1u) == 0u && operation >= 0x22u)
        {
            // These variants are I-dependent; handled below.
            if (operation == 0x1du || operation == 0x1fu)
            {
                return false;
            }
            operand = state.m_i;
        }
        else if (operation == 0x22u || operation == 0x23u ||
                 operation == 0x26u || operation == 0x27u)
        {
            operand = state.m_i;
        }
        else
        {
            return false;
        }
        broadcast = true;
    }

    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        if ((mask & (8u >> lane)) == 0u)
        {
            continue;
        }
        const uint32_t lhs = state.m_vf[fs][lane];
        const uint32_t rhs = broadcast
            ? (operation >= 0x1cu ? operand : state.m_vf[ft][funct & 3u])
            : state.m_vf[ft][lane];
        uint32_t result = 0u;
        if (operation <= 0x03u || operation == 0x28u || operation == 0x22u)
        {
            result = to_bits(to_float(lhs) + to_float(rhs));
        }
        else if (operation <= 0x07u || operation == 0x2cu || operation == 0x26u)
        {
            result = to_bits(to_float(lhs) - to_float(rhs));
        }
        else if ((operation >= 0x18u && operation <= 0x1bu) ||
                 operation == 0x2au || operation == 0x1eu)
        {
            result = to_bits(to_float(lhs) * to_float(rhs));
        }
        else if ((operation >= 0x10u && operation <= 0x13u) ||
                 operation == 0x2bu)
        {
            result = to_float(lhs) > to_float(rhs) ? lhs : rhs;
        }
        else if ((operation >= 0x14u && operation <= 0x17u) ||
                 operation == 0x2fu)
        {
            result = to_float(lhs) < to_float(rhs) ? lhs : rhs;
        }
        else
        {
            // MADD/MSUB/ACC/MAC/Q operations not safe without pipelines.
            return false;
        }
        write.m_values[lane] = result;
    }
    return true;
}

bool lower(s_ee_vu1_state& state, uint32_t lo, uint32_t pc,
           std::array<uint8_t, 16384>& data, uint32_t top, uint32_t itop,
           const std::array<uint16_t, 16>& branch_vi,
           std::vector<uint8_t>& path1_bytes,
           uint32_t& branch_target, bool& branched)
{
    const uint32_t major = lo >> 25u;
    const uint32_t dest = (lo >> 21u) & 15u;
    const uint32_t it = (lo >> 16u) & 15u;
    const uint32_t is = (lo >> 11u) & 15u;
    const uint32_t id = (lo >> 6u) & 15u;
    const uint32_t ft = (lo >> 16u) & 31u;
    const uint32_t fs = (lo >> 11u) & 31u;
    const int32_t imm11 = static_cast<int32_t>(lo << 21u) >> 21u;
    const uint32_t target = pc + 8u +
        static_cast<uint32_t>(imm11) * 8u;

    if (major == 0x40u)
    {
        const uint32_t funct = lo & 63u;
        if (funct < 0x3cu)
        {
            switch (funct)
            {
            case 0x30u: store_vi(state, id, state.m_vi[is] + state.m_vi[it]); return true;
            case 0x31u: store_vi(state, id, state.m_vi[is] - state.m_vi[it]); return true;
            case 0x32u:
                store_vi(state, it, state.m_vi[is] +
                    (static_cast<int32_t>(((lo >> 6u) & 31u) << 27u) >> 27u));
                return true;
            case 0x34u: store_vi(state, id, state.m_vi[is] & state.m_vi[it]); return true;
            case 0x35u: store_vi(state, id, state.m_vi[is] | state.m_vi[it]); return true;
            default: return false;
            }
        }
        const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (funct & 3u);
        switch (index)
        {
        case 0x30u: // MOVE
        case 0x31u: // MR32
            if (ft != 0u)
            {
                const auto source = state.m_vf[fs];
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = source[
                            index == 0x30u ? lane : (lane + 1u) & 3u];
                    }
                }
            }
            return true;
        case 0x68u: store_vi(state, it, top); return true;
        case 0x69u: store_vi(state, it, itop); return true;
        case 0x6cu: return xgkick(data, state.m_vi[is], path1_bytes);
        default: return false;
        }
    }

    if (major == 0x00u || major == 0x01u)
    {
        uint8_t* bytes = data.data() + qword_offset(
            state.m_vi[major == 0u ? is : it] +
            static_cast<uint32_t>(imm11));
        if (major == 0u)
        {
            if (ft != 0u)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = read32(bytes + lane * 4u);
                    }
                }
            }
        }
        else
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((dest & (8u >> lane)) != 0u)
                {
                    const uint32_t value = state.m_vf[fs][lane];
                    std::memcpy(bytes + lane * 4u, &value, sizeof(value));
                }
            }
        }
        return true;
    }

    switch (major)
    {
    case 0x08u:
        store_vi(state, it, state.m_vi[is] +
            (((lo >> 10u) & 0x7800u) | (lo & 0x7ffu)));
        return true;
    case 0x09u:
        store_vi(state, it, state.m_vi[is] -
            (((lo >> 10u) & 0x7800u) | (lo & 0x7ffu)));
        return true;
    case 0x20u: branch_target = target; branched = true; return true;
    case 0x21u:
        store_vi(state, it, (pc + 16u) / 8u);
        branch_target = target;
        branched = true;
        return true;
    case 0x24u: branch_target = state.m_vi[is] * 8u; branched = true; return true;
    case 0x25u:
        branch_target = state.m_vi[is] * 8u;
        store_vi(state, it, (pc + 16u) / 8u);
        branched = true;
        return true;
    case 0x28u: branched = branch_vi[is] == branch_vi[it]; break;
    case 0x29u: branched = branch_vi[is] != branch_vi[it]; break;
    case 0x2cu: branched = static_cast<int16_t>(branch_vi[is]) < 0; break;
    case 0x2du: branched = static_cast<int16_t>(branch_vi[is]) > 0; break;
    case 0x2eu: branched = static_cast<int16_t>(branch_vi[is]) <= 0; break;
    case 0x2fu: branched = static_cast<int16_t>(branch_vi[is]) >= 0; break;
    default: return false;
    }
    if (branched)
    {
        branch_target = target;
    }
    return true;
}
} // namespace

bool c_ee_vu1::run(
    s_ee_vu1_state& state,
    std::array<uint8_t, k_memory_size>& code,
    std::array<uint8_t, k_memory_size>& data,
    uint32_t address, uint32_t top, uint32_t itop,
    std::vector<uint8_t>& path1_bytes, s_ee_vu1_issue* issue)
{
    if (issue == nullptr || (address & 7u) != 0u)
    {
        return false;
    }
    *issue = {};
    if (!state.m_initialized)
    {
        state = {};
        state.m_vf[0][3] = 0x3f800000u;
        state.m_initialized = true;
    }

    uint32_t pc = address & 0x3ff8u;
    uint32_t npc = (pc + 8u) & 0x3fffu;
    std::array<uint16_t, 16> previous_vi = state.m_vi;
    bool previous_lower = false;
    bool ending = false;

    for (uint32_t step = 0u; step < k_max_steps; ++step)
    {
        const uint32_t lo = read32(code.data() + pc);
        const uint32_t hi = read32(code.data() + pc + 4u);
        issue->m_pc = pc;
        issue->m_lower = lo;
        issue->m_upper = hi;
        issue->m_steps = step + 1u;

        s_upper_write pending{};
        if (!upper(state, hi, pending))
        {
            return false;
        }

        const auto before = state.m_vi;
        const uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & 0x3fffu;

        if ((hi & 0x80000000u) != 0u)
        {
            state.m_i = lo;
        }
        else
        {
            uint32_t target = 0u;
            bool branched = false;
            if (!lower(state, lo, current, data, top, itop,
                       previous_lower ? previous_vi : state.m_vi,
                       path1_bytes, target, branched))
            {
                return false;
            }
            if (branched)
            {
                npc = target & 0x3fffu;
            }
        }

        if (pending.m_target != 0u)
        {
            auto& target = pending.m_acc
                ? state.m_acc : state.m_vf[pending.m_target];
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((pending.m_mask & (8u >> lane)) != 0u)
                {
                    target[lane] = pending.m_values[lane];
                }
            }
        }
        state.m_vi[0] = 0u;
        state.m_vf[0] = {0u, 0u, 0u, 0x3f800000u};
        previous_vi = before;
        previous_lower = (hi & 0x80000000u) == 0u;

        if (ending)
        {
            state.m_tpc = pc;
            return true;
        }
        if ((hi & 0x40000000u) != 0u)
        {
            ending = true; // E-bit takes effect AFTER the delay-slot pair.
        }
    }
    return false; // Real E-bit never reached within bounded execution.
}
} // namespace ee
