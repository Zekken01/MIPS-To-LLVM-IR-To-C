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

void c_ee_dmac::bind_memory(ee_memory* memory)
{
    m_memory = memory;
}

void c_ee_dmac::reset()
{
    m_status = 0u;
    for (s_spr_channel& channel : m_spr_channels)
    {
        channel = {};
    }
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

bool c_ee_dmac::read_spr_channel(uint32_t address, uint32_t* value) const
{
    if (value == nullptr)
    {
        return false;
    }

    const s_spr_channel* channel = nullptr;
    uint32_t offset = 0;
    if (address >= k_from_spr_base && address < k_from_spr_base + 0x100u)
    {
        channel = &m_spr_channels[0];
        offset = address - k_from_spr_base;
    }
    else if (address >= k_to_spr_base && address < k_to_spr_base + 0x100u)
    {
        channel = &m_spr_channels[1];
        offset = address - k_to_spr_base;
    }
    else
    {
        return false;
    }

    switch (offset)
    {
    case 0x00u: *value = channel->m_chcr; return true;
    case 0x10u: *value = channel->m_madr; return true;
    case 0x20u: *value = channel->m_qwc; return true;
    case 0x30u: *value = channel->m_tadr; return true;
    case 0x80u: *value = channel->m_sadr; return true;
    default: return false;
    }
}

bool c_ee_dmac::transfer_spr_normal(uint32_t channel_index)
{
    if (m_memory == nullptr || channel_index < 8u || channel_index > 9u)
    {
        return false;
    }

    s_spr_channel& channel = m_spr_channels[channel_index - 8u];
    // At this milestone, only normal DMA is implemented. Leaving STR set
    // on unsupported modes or invalid memory prevents a false completion.
    if (((channel.m_chcr >> 2u) & 3u) != 0u)
    {
        return false;
    }

    const size_t bytes = static_cast<size_t>(channel.m_qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;

    // DMA addresses here are *physical main RAM*, not guest KSEG aliases
    // or a disguised pointer into SPR/MMIO. Validate the entire transfer
    // before any write to avoid partially completing a bad channel.
    if ((channel.m_madr & 0x0fu) != 0u ||
        channel.m_madr >= 0x02000000u ||
        end > 0x02000000ull ||
        !m_memory->contains(channel.m_madr, bytes))
    {
        return false;
    }

    for (uint32_t i = 0; i < channel.m_qwc; ++i)
    {
        const uint32_t ram_address = channel.m_madr + i * 16u;
        const uint32_t spr_offset = (channel.m_sadr + i * 16u) & 0x3ff0u;
        const uint32_t spr_address = ee_memory::k_scratchpad_base + spr_offset;
        s_ee_u128 quad{};

        if (channel_index == 8u) // fromSPR: SPR -> EE RAM
        {
            m_memory->read128(spr_address, &quad);
            m_memory->write128(ram_address, &quad);
        }
        else // toSPR: EE RAM -> SPR
        {
            m_memory->read128(ram_address, &quad);
            m_memory->write128(spr_address, &quad);
        }
    }

    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_sadr = (channel.m_sadr + static_cast<uint32_t>(bytes)) & 0x3ff0u;
    channel.m_qwc = 0u;
    channel.m_chcr &= ~k_chcr_str;
    // The existing D_STAT model latches completion even if masked, and
    // asserts COP0 IP3 only when the matching mask bit is enabled.
    return raise(channel_index);
}

bool c_ee_dmac::write_spr_channel(uint32_t address, uint32_t value)
{
    s_spr_channel* channel = nullptr;
    uint32_t channel_index = 0;
    uint32_t offset = 0;

    if (address >= k_from_spr_base && address < k_from_spr_base + 0x100u)
    {
        channel = &m_spr_channels[0];
        channel_index = 8u;
        offset = address - k_from_spr_base;
    }
    else if (address >= k_to_spr_base && address < k_to_spr_base + 0x100u)
    {
        channel = &m_spr_channels[1];
        channel_index = 9u;
        offset = address - k_to_spr_base;
    }
    else
    {
        return false;
    }

    switch (offset)
    {
    case 0x00u:
    {
        const bool was_running = (channel->m_chcr & k_chcr_str) != 0u;
        channel->m_chcr = value;
        if (!was_running && (value & k_chcr_str) != 0u)
        {
            (void)transfer_spr_normal(channel_index);
        }
        return true;
    }
    case 0x10u: channel->m_madr = value; return true;
    case 0x20u: channel->m_qwc = value & 0xffffu; return true;
    case 0x30u: channel->m_tadr = value; return true;
    case 0x80u: channel->m_sadr = value & 0x3ff0u; return true;
    default: return false;
    }
}

bool c_ee_dmac::mmio_read(void* user, uint32_t address,
                           uint32_t width, s_ee_u128* value)
{
    if (user == nullptr || value == nullptr || width != 4u)
    {
        return false;
    }

    c_ee_dmac* controller = static_cast<c_ee_dmac*>(user);
    uint32_t word = 0u;
    if (address == k_stat_address)
    {
        word = controller->m_status;
    }
    else if (!controller->read_spr_channel(address, &word))
    {
        return false;
    }

    *value = {word, 0u};
    return true;
}

bool c_ee_dmac::mmio_write(void* user, uint32_t address,
                            uint32_t width, const s_ee_u128* value)
{
    if (user == nullptr || value == nullptr || width != 4u)
    {
        return false;
    }

    c_ee_dmac* controller = static_cast<c_ee_dmac*>(user);
    const uint32_t word = static_cast<uint32_t>(value->m_lo);
    if (address != k_stat_address)
    {
        return controller->write_spr_channel(address, word);
    }

    // D_STAT lower 16 bits are W1C, upper 16 bits W1-toggle.
    // Keep control/other D_STAT bits visible without pretending that
    // a transfer channel is implemented.
    controller->m_status &= ~(word & 0x0000ffffu);
    controller->m_status ^= word & 0xffff0000u;
    controller->sync_interrupt_line();
    return true;
}
} // namespace ee
