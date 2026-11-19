// Derived from ps2-web-recomp:SDBZ runtime/src/vu_micro.cpp
// (Samir Nuri, 2026, MIT). See ps2_webgpu/gs/LICENSE.upstream.
// This first batch intentionally rejects unported instructions and timing
// dependencies instead of silently treating them as NOPs.
#include "ee_vu1.h"
#include "ee_vu1_fp.h"

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
    uint32_t m_clip = 0u;
    bool m_acc = false;
    bool m_flags = false;
    bool m_has_clip = false;
    bool m_writes = false;
};

// VU FMAC and status calculations ported from vu_micro.cpp::set_flags.
void set_mac_flags(s_ee_vu1_state& state,
                   const std::array<uint32_t, 4>& value, uint32_t mask)
{
    uint32_t mac = 0u;
    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        if ((mask & (8u >> lane)) == 0u)
        {
            continue;
        }
        const uint32_t bit = 3u - lane;
        const uint32_t x = value[lane];
        if ((x & 0x7f800000u) == 0u)
        {
            mac |= 1u << bit;
        }
        if ((x & 0x80000000u) != 0u)
        {
            mac |= 1u << (bit + 4u);
        }
        if ((x & 0x7fffffffu) == 0x7fffffffu)
        {
            mac |= 1u << (bit + 12u);
        }
    }
    state.m_mac = mac;
    uint32_t status = state.m_status & 0xff0u;
    if ((mac & 0x000fu) != 0u) { status |= 1u; }
    if ((mac & 0x00f0u) != 0u) { status |= 2u; }
    if ((mac & 0x0f00u) != 0u) { status |= 4u; }
    if ((mac & 0xf000u) != 0u) { status |= 8u; }
    status |= (status & 15u) << 6u;
    state.m_status = (status & ~0x30u) | (state.m_status & 0x30u);
}

// All FMAC mathematics and special-index mappings below are ported from
// ps2-web-recomp's reference upper()/decode_upper(). No Q-dependent opcode
// is enabled before the Q-latency pipeline is implemented.
bool upper(const s_ee_vu1_state& state, uint32_t hi,
           s_upper_write& write)
{
    namespace fp = vu1_fp;
    const uint32_t funct = hi & 63u;
    const bool special = funct >= 0x3cu;
    const uint32_t index = (((hi >> 6u) & 31u) << 2u) | (funct & 3u);
    const uint32_t op = special ? index : funct;
    const uint32_t mask = (hi >> 21u) & 15u;
    const uint32_t fs = (hi >> 11u) & 31u;
    const uint32_t ft = (hi >> 16u) & 31u;
    const uint32_t fd = (hi >> 6u) & 31u;

    if (special && op == 0x2fu)
    {
        return true; // Canonical upper NOP.
    }
    if (op > 0x2fu || (!special && funct >= 0x30u) ||
        op == 0x1cu || op == 0x20u || op == 0x21u ||
        op == 0x24u || op == 0x25u)
    {
        return false; // Unknown opcode or Q-latency dependency.
    }

    const bool conversion = special && op >= 0x10u && op <= 0x17u;
    const bool abs_op = special && op == 0x1du;
    const bool clip_op = special && op == 0x1fu;
    const bool cross = op == 0x2eu;
    const bool uses_acc = special && !conversion && !abs_op &&
        !clip_op && op != 0x2fu;
    const bool is_minmax =
        (op >= 0x10u && op <= 0x17u && !conversion) ||
        op == 0x1du || op == 0x1fu || op == 0x2bu || op == 0x2fu;
    const bool is_arithmetic = !conversion && !abs_op &&
        !clip_op && !is_minmax;

    if (clip_op)
    {
        const int32_t limit = fp::key(state.m_vf[ft][3] & fp::kAbs);
        uint32_t flags = 0u;
        for (uint32_t lane = 0u; lane < 3u; ++lane)
        {
            const int32_t x = fp::key(state.m_vf[fs][lane]);
            if (x > limit) { flags |= 1u << (lane * 2u); }
            if (x < -limit) { flags |= 2u << (lane * 2u); }
        }
        write.m_clip = ((state.m_clip << 6u) | flags) & 0xffffffu;
        write.m_has_clip = true;
        return true;
    }

    write.m_mask = cross ? 0xeu : mask;
    write.m_acc = uses_acc;
    write.m_target = special && (conversion || abs_op) ? ft : fd;
    write.m_flags = is_arithmetic;
    write.m_writes = true;
    static constexpr int k_fractions[4]{0, 4, 12, 15};
    const int frac = k_fractions[op & 3u];

    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        if ((write.m_mask & (8u >> lane)) == 0u)
        {
            continue;
        }
        const uint32_t lhs = state.m_vf[fs][cross
            ? (lane == 0u ? 1u : lane == 1u ? 2u : 0u) : lane];
        const uint32_t rhs_cross = state.m_vf[ft][
            lane == 0u ? 2u : lane == 1u ? 0u : 1u];
        const uint32_t rhs =
            op <= 0x1bu ? state.m_vf[ft][funct & 3u] :
            op == 0x1du || op == 0x1eu || op == 0x1fu ||
            op == 0x22u || op == 0x23u || op == 0x26u ||
            op == 0x27u ? state.m_i : state.m_vf[ft][lane];
        const uint32_t acc = state.m_acc[lane];
        uint32_t result = 0u;

        if (conversion)
        {
            if (op <= 0x13u)
            {
                result = fp::from_fixed(
                    static_cast<int32_t>(lhs), frac);
            }
            else
            {
                result = fp::to_fixed(lhs, frac);
            }
        }
        else if (abs_op)
        {
            result = lhs & fp::kAbs;
        }
        else if (cross)
        {
            const uint32_t product = fp::mul(lhs, rhs_cross);
            result = special ? product : fp::sub(acc, product);
        }
        else if (op <= 0x03u || op == 0x20u ||
                 op == 0x22u || op == 0x28u)
        {
            result = fp::add(lhs, rhs);
        }
        else if ((op >= 0x04u && op <= 0x07u) ||
                 op == 0x24u || op == 0x26u || op == 0x2cu)
        {
            result = fp::sub(lhs, rhs);
        }
        else if ((op >= 0x08u && op <= 0x0bu) ||
                 op == 0x21u || op == 0x23u || op == 0x29u)
        {
            result = fp::add(acc, fp::mul(lhs, rhs));
        }
        else if ((op >= 0x0cu && op <= 0x0fu) ||
                 op == 0x25u || op == 0x27u || op == 0x2du)
        {
            result = fp::sub(acc, fp::mul(lhs, rhs));
        }
        else if ((op >= 0x10u && op <= 0x13u) ||
                 op == 0x1du || op == 0x2bu)
        {
            result = fp::max(lhs, rhs);
        }
        else if ((op >= 0x14u && op <= 0x17u) ||
                 op == 0x1fu || op == 0x2fu)
        {
            result = fp::min(lhs, rhs);
        }
        else if ((op >= 0x18u && op <= 0x1bu) ||
                 op == 0x1eu || op == 0x2au)
        {
            result = fp::mul(lhs, rhs);
        }
        else
        {
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
