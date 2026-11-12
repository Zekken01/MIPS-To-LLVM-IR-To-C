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

    switch (index)
    {
    case 16: return vu0->m_status_flags;
    case 17: return vu0->m_mac_flags;
    case 18: return vu0->m_clip_flags;
    case 26: return vu0->m_tpc;
    case 28: return vu0->m_fbrst;
    case 29: return vu0->m_vpu_status;
    default: return vu0->m_vi[index];
    }
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
    switch (index)
    {
    case 16: vu0->m_status_flags = value; break;
    case 17: vu0->m_mac_flags = value; break;
    case 18: vu0->m_clip_flags = value; break;
    case 26: vu0->m_tpc = value; break;
    case 28: vu0->m_fbrst = value; break;
    case 29: vu0->m_vpu_status = value; break;
    default: break;
    }
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


extern "C" uint32_t ee_vu0_ctc2(
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

    const uint32_t sync_result = ee_vu0_sync(cpu, rt, mode, pc);
    if (sync_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return sync_result;
    }

    if (vi_index == 0)
    {
        return sync_result;
    }

    // The source is the low 32-bit word; GPR0 is hardwired to zero even if a
    // malformed host state has non-zero backing storage for it.
    const uint32_t value = gpr_index == 0
        ? 0
        : static_cast<uint32_t>(cpu->m_gpr[gpr_index].m_lo);

    s_ee_vu0_state* vu0 = rt->m_vu0_state;
    switch (vi_index)
    {
    case 17: // MAC_FLAG is read-only.
    case 26: // TPC is read-only.
    case 29: // VPU_STAT is read-only.
        return sync_result;

    case 20: // R: preserve the implicit 1.0 exponent.
        ee_vu0_write_vi(vu0, vi_index, (value & VU0_R_MASK) | 0x3f800000u);
        return sync_result;

    case 28: // FBRST: mask stored bits and route reset/break actions to host.
    {
        constexpr uint32_t FBRST_ACTION_MASK = 0x00000303u;
        if ((value & FBRST_ACTION_MASK) != 0)
        {
            if (!rt->m_vu0_control)
            {
                return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
            }

            const uint32_t control_result = rt->m_vu0_control(
                rt->m_vu0_control_user, cpu, vu0, vi_index, value, pc
            );
            if (control_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
            {
                return control_result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
                    ? control_result
                    : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
            }
        }

        ee_vu0_write_vi(vu0, vi_index, value & 0x00000c0cu);
        return sync_result;
    }

    case 31: // CMSAR1 starts a VU1 micro subroutine after VU1 is finished.
    {
        if (!rt->m_vu0_control)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }

        const uint32_t control_result = rt->m_vu0_control(
            rt->m_vu0_control_user, cpu, vu0, vi_index, value & 0xffffu, pc
        );
        if (control_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return control_result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
                ? control_result
                : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }

        vu0->m_cmsar1 = value & 0xffffu;
        return sync_result;
    }

    case 18: // CLIP_FLAG also updates the separately tracked clip flags.
        vu0->m_clip_flags = value;
        ee_vu0_write_vi(vu0, vi_index, value);
        return sync_result;

    default:
        ee_vu0_write_vi(vu0, vi_index, value);
        return sync_result;
    }
}

} // namespace ee
