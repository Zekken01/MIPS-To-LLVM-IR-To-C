#include "ee_vsync_flags.h"
#include "ee_memory.h"

namespace ee
{

bool c_ee_vsync_flags::register_guest_pointers(
    const ee_memory* memory,
    uint32_t flag_address,
    uint32_t csr_address
)
{
    bool accepted = memory != nullptr;
    if (accepted && flag_address != 0)
    {
        accepted = (flag_address & 3u) == 0u &&
            memory->contains(flag_address, sizeof(uint32_t));
    }
    if (accepted && csr_address != 0)
    {
        accepted = (csr_address & 7u) == 0u &&
            memory->contains(csr_address, sizeof(uint64_t));
    }
    if (accepted)
    {
        // No writes occur during registration. Keep both old addresses on
        // refusal, so a failed re-registration does not partially replace
        // a previously accepted configuration.
        m_flag_address = flag_address;
        m_csr_address = csr_address;
    }
    return accepted;
}

bool c_ee_vsync_flags::deliver_actual_vsync(
    ee_memory* memory,
    uint64_t gs_csr
)
{
    if (memory == nullptr ||
        (m_flag_address != 0 &&
         !memory->contains(m_flag_address, sizeof(uint32_t))) ||
        (m_csr_address != 0 &&
         !memory->contains(m_csr_address, sizeof(uint64_t))))
    {
        return false;
    }

    // This is deliberately not called by the HLE registration syscall.
    // A future GS event source must pass the CSR sampled at *real* VSync.
    if (m_flag_address != 0)
    {
        memory->write32(m_flag_address, 1u);
    }
    if (m_csr_address != 0)
    {
        memory->write64(m_csr_address, gs_csr);
    }
    return true;
}

uint32_t c_ee_vsync_flags::flag_address() const
{
    return m_flag_address;
}

uint32_t c_ee_vsync_flags::csr_address() const
{
    return m_csr_address;
}

} // namespace ee
