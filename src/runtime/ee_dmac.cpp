#include "ee_dmac.h"
#include "ee_exceptions.h"

namespace ee
{
void c_ee_dmac::sync_interrupt_line()
{
    if (m_cpu == nullptr)
    {
        return;
    }

    if (pending() != 0u)
    {
        ee_interrupt_request(m_cpu, INT_DMAC);
    }
    else
    {
        ee_interrupt_clear(m_cpu, INT_DMAC);
    }
}

void c_ee_dmac::bind_cpu(s_ee_cpu_state* cpu)
{
    if (m_cpu != nullptr && m_cpu != cpu)
    {
        ee_interrupt_clear(m_cpu, INT_DMAC);
    }

    m_cpu = cpu;
    sync_interrupt_line();
}

void c_ee_dmac::reset()
{
    m_status = 0u;
    sync_interrupt_line();
}

bool c_ee_dmac::raise(uint32_t channel)
{
    if (channel >= k_channel_count)
    {
        return false;
    }

    m_status |= uint32_t{1} << channel;
    sync_interrupt_line();
    return true;
}

bool c_ee_dmac::acknowledge(uint32_t channel)
{
    if (channel >= k_channel_count)
    {
        return false;
    }

    m_status &= ~(uint32_t{1} << channel);
    sync_interrupt_line();
    return true;
}

uint32_t c_ee_dmac::status() const
{
    return m_status;
}

uint32_t c_ee_dmac::pending() const
{
    // Physical DMA channels 0..9: low status bits and high mask bits.
    // This is the same CIS & CIM expression used in ps2-web-recomp.
    return (m_status & 0x03ffu) & ((m_status >> 16u) & 0x03ffu);
}

bool c_ee_dmac::mmio_read(void* user, uint32_t address,
                           uint32_t width, s_ee_u128* value)
{
    if (user == nullptr || value == nullptr ||
        address != k_stat_address || width != 4u)
    {
        return false;
    }

    c_ee_dmac* controller = static_cast<c_ee_dmac*>(user);
    *value = {controller->m_status, 0u};
    return true;
}

bool c_ee_dmac::mmio_write(void* user, uint32_t address,
                            uint32_t width, const s_ee_u128* value)
{
    if (user == nullptr || value == nullptr ||
        address != k_stat_address || width != 4u)
    {
        return false;
    }

    c_ee_dmac* controller = static_cast<c_ee_dmac*>(user);
    const uint32_t word = static_cast<uint32_t>(value->m_lo);

    // D_STAT lower 16 bits are W1C, upper 16 bits W1-toggle.
    // Keep control/other D_STAT bits visible without pretending that
    // a transfer channel is implemented.
    controller->m_status &= ~(word & 0x0000ffffu);
    controller->m_status ^= word & 0xffff0000u;
    controller->sync_interrupt_line();
    return true;
}
} // namespace ee
