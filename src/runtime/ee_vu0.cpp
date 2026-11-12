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

} // namespace ee
