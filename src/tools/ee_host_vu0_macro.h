#pragma once

#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
// HLE-first bridge for genuine R5900 VU0 COP2 indexed vector-memory
// instructions. The native lifter deliberately exits as UNSUPPORTED for
// these operations; this host-owned fallback executes only four verified
// SPECIAL2 opcodes without changing the existing translated-function cache.
// Follow ps2-web-recomp's VU addressing/VI rules and the in-tree micro
// interpreter's quadword lane transfer semantics.
class c_ee_host_vu0_macro
{
public:
    static bool indexed_memory_opcode(uint32_t word)
    {
        if ((word >> 26u) != 0x12u ||
            ((word >> 21u) & 31u) < 0x10u ||
            (word & 0x3cu) != 0x3cu)
        {
            return false;
        }
        const uint32_t secondary = (word & 3u) | ((word >> 4u) & 0x7cu);
        return secondary >= 0x34u && secondary <= 0x37u;
    }

    static bool execute(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                        uint32_t word, uint32_t pc)
    {
        if (cpu == nullptr || rt == nullptr || rt->m_vu0_state == nullptr ||
            !indexed_memory_opcode(word))
        {
            return false;
        }
        const uint32_t secondary = (word & 3u) | ((word >> 4u) & 0x7cu);
        const bool load = (secondary & 1u) == 0u;
        const bool predecrement = (secondary & 2u) != 0u;
        const uint32_t fs = (word >> 11u) & 31u;
        const uint32_t ft = (word >> 16u) & 31u;
        const uint32_t base_index = load ? fs : ft;
        const uint32_t vf_index = load ? ft : fs;
        const uint32_t mask = (word >> 21u) & 15u;

        // VU memory base registers are VI0..VI15. The register bank
        // above VI15 contains status/control registers, not addresses.
        if (base_index >= 16u)
        {
            return false;
        }

        // COP2 macro instructions must finish an active VU0 microprogram
        // before modifying VF/VI/data memory. A failed synchronization
        // must have no side effects and is not a success-shaped no-op.
        if (ee_vu0_sync(cpu, rt,
                static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E), pc) !=
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return false;
        }

        s_ee_vu0_state* vu0 = rt->m_vu0_state;
        uint16_t vi_value = static_cast<uint16_t>(
            ee_vu0_read_vi(vu0, base_index));
        if (predecrement && base_index != 0u)
        {
            vi_value = static_cast<uint16_t>(vi_value - 1u);
        }

        // VI addresses count quadwords; VU0 has 256 quadwords of data
        // memory. Apply the architectural 4 KiB wrap before lane access.
        const uint32_t address = (static_cast<uint32_t>(vi_value) << 4u) &
            (EE_VU0_DATA_MEMORY_SIZE - 1u);
        const s_ee_u128 original = vf_index == 0u
            ? s_ee_u128{0u, 0x3f80000000000000ull}
            : cpu->m_vu0[vf_index];
        s_ee_u128 result = original;

        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) == 0u)
            {
                continue;
            }
            const uint32_t offset = address + 4u * lane;
            const uint32_t shift = (lane & 1u) * 32u;
            uint64_t& half = lane < 2u ? result.m_lo : result.m_hi;
            if (load)
            {
                uint32_t bits = 0u;
                for (uint32_t byte = 0u; byte < 4u; ++byte)
                {
                    bits |= static_cast<uint32_t>(
                        vu0->m_data_memory[offset + byte]) << (byte * 8u);
                }
                half = (half & ~(0xffffffffull << shift)) |
                    (static_cast<uint64_t>(bits) << shift);
            }
            else
            {
                const uint32_t bits = static_cast<uint32_t>(half >> shift);
                for (uint32_t byte = 0u; byte < 4u; ++byte)
                {
                    vu0->m_data_memory[offset + byte] =
                        static_cast<uint8_t>(bits >> (byte * 8u));
                }
            }
        }

        if (load && vf_index != 0u)
        {
            cpu->m_vu0[vf_index] = result;
        }
        // Increment/decrement is required even for VF0 and mask=0.
        // VI0 is constant zero. Low 16 bits of the remaining VI registers
        // carry the integer address, including wraparound.
        if (base_index != 0u)
        {
            ee_vu0_write_vi(vu0, base_index,
                predecrement ? vi_value
                : static_cast<uint16_t>(vi_value + 1u));
        }
        return true;
    }
};
} // namespace ee
