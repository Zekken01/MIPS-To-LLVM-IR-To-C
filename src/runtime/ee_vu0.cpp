#include "ee_runtime.h"

namespace ee
{

e_ee_vu0_sync_mode ee_vu0_transfer_sync_mode(
    e_ee_vu0_transfer_direction direction,
    bool in_delay_slot
)
{
    if (!in_delay_slot)
    {
        return e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE;
    }

    return direction == e_ee_vu0_transfer_direction::FROM_VU0
        ? e_ee_vu0_sync_mode::FINISH_TO_E
        : e_ee_vu0_sync_mode::WAIT_TO_E_OR_M;
}

extern "C" uint32_t ee_vu0_sync(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || !rt ||
        mode > static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (!rt->m_vu0_state || !rt->m_vu0_sync)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const uint32_t result = rt->m_vu0_sync(
        rt->m_vu0_user, cpu, rt->m_vu0_state, mode, pc
    );

    if (result > static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    return result;
}

extern "C" uint32_t ee_vu0_read_vi(
    const s_ee_vu0_state* vu0,
    uint32_t index
)
{
    if (!vu0 || index >= 32 || index == 0)
    {
        return 0;
    }

    return vu0->m_vi[index];
}

extern "C" void ee_vu0_write_vi(
    s_ee_vu0_state* vu0,
    uint32_t index,
    uint32_t value
)
{
    if (!vu0 || index >= 32 || index == 0)
    {
        return;
    }

    vu0->m_vi[index] = value;
}


namespace
{
constexpr uint32_t VU0_REG_R = 20;
constexpr uint32_t VU0_R_MASK = 0x007fffffu;
} // namespace

extern "C" uint32_t ee_vu0_qmfc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vf_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vf_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (gpr_index != 0)
    {
        // VF0 is architecturally constant: (0, 0, 0, 1.0f). Do not trust
        // the backing array for register zero, which must never be written.
        cpu->m_gpr[gpr_index] = vf_index == 0
            ? s_ee_u128{0, 0x3f80000000000000ULL}
            : cpu->m_vu0[vf_index];
    }

    return result;
}

extern "C" uint32_t ee_vu0_qmtc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vf_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vf_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (vf_index != 0)
    {
        cpu->m_vu0[vf_index] = gpr_index == 0
            ? s_ee_u128{}
            : cpu->m_gpr[gpr_index];
    }

    return result;
}

extern "C" uint32_t ee_vu0_cfc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vi_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vi_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (gpr_index != 0)
    {
        uint32_t value = ee_vu0_read_vi(rt->m_vu0_state, vi_index);
        if (vi_index == VU0_REG_R)
        {
            // PCSX2 writes only UL[0] on this special path. Preserve UL[1]
            // rather than sign-extending or zero-extending the 23-bit value.
            cpu->m_gpr[gpr_index].m_lo =
                (cpu->m_gpr[gpr_index].m_lo & 0xffffffff00000000ULL) |
                (static_cast<uint64_t>(value) & VU0_R_MASK);
        }
        else
        {
            // All ordinary VI reads sign-extend UL[0] into UL[1].
            cpu->m_gpr[gpr_index].m_lo =
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
        }
    }

    return result;
}

} // namespace ee
