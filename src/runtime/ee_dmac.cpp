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
    m_sqwc = 0u;
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
    // ps2-web-recomp falls back to contiguous normal DMA for MOD=2
    // when D_SQWC.TQWC is zero. The modulo operation must never
    // be executed with a zero transfer period.
    const uint32_t mode = (channel.m_chcr >> 2u) & 3u;
    if (mode != 0u && !(mode == 2u && ((m_sqwc >> 16u) & 0xffu) == 0u))
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

bool c_ee_dmac::transfer_spr_interleave(uint32_t channel_index)
{
    if (m_memory == nullptr || channel_index < 8u || channel_index > 9u)
    {
        return false;
    }

    s_spr_channel& channel = m_spr_channels[channel_index - 8u];
    const uint32_t transfer = (m_sqwc >> 16u) & 0xffu;
    const uint32_t skip = m_sqwc & 0xffu;

    if (((channel.m_chcr >> 2u) & 3u) != 2u || transfer == 0u)
    {
        return false;
    }

    // Match the reference's per-QWC D_SQWC pattern: one RAM qword at
    // MADR, then advance by 16 bytes; after each group of TQWC words,
    // also skip SQWC words in RAM. SADR always advances contiguously
    // through SPR, wrapping at the 16 KiB boundary.
    //
    // Preflight EVERY RAM qword before writing anything: a contiguous
    // bounds check is insufficient because the skipped RAM holes may
    // carry a later transfer beyond the allocated EE memory.
    uint64_t address = channel.m_madr;
    for (uint32_t i = 0; i < channel.m_qwc; ++i)
    {
        if ((address & 0x0fu) != 0u ||
            address >= 0x02000000ull ||
            address + 16u > 0x02000000ull ||
            !m_memory->contains(static_cast<uint32_t>(address), 16u))
        {
            return false;
        }

        address += 16u;
        if ((i + 1u) % transfer == 0u)
        {
            address += static_cast<uint64_t>(skip) * 16u;
        }
    }

    // The validated RAM addresses and SPR qwords are independent.
    // Copy only after the complete interleave pattern has passed.
    uint64_t current = channel.m_madr;
    for (uint32_t i = 0; i < channel.m_qwc; ++i)
    {
        const uint32_t ram_address = static_cast<uint32_t>(current);
        const uint32_t spr_address = ee_memory::k_scratchpad_base +
            ((channel.m_sadr + i * 16u) & 0x3ff0u);
        s_ee_u128 quad{};

        if (channel_index == 8u)
        {
            m_memory->read128(spr_address, &quad);
            m_memory->write128(ram_address, &quad);
        }
        else
        {
            m_memory->read128(ram_address, &quad);
            m_memory->write128(spr_address, &quad);
        }

        current += 16u;
        if ((i + 1u) % transfer == 0u)
        {
            current += static_cast<uint64_t>(skip) * 16u;
        }
    }

    channel.m_madr = static_cast<uint32_t>(current);
    channel.m_sadr = (channel.m_sadr + channel.m_qwc * 16u) & 0x3ff0u;
    channel.m_qwc = 0u;
    channel.m_chcr &= ~k_chcr_str;
    return raise(channel_index);
}

bool c_ee_dmac::transfer_from_spr_span(
    s_spr_channel& channel, uint32_t qwc, bool copy_data)
{
    // DMA MADR is a physical EE address. Match the normal-mode policy:
    // reject unbacked, unaligned, or non-RAM spans without any writes.
    const uint64_t bytes = static_cast<uint64_t>(qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;

    if (qwc != 0u &&
        ((channel.m_madr & 0x0fu) != 0u ||
         channel.m_madr >= 0x02000000u ||
         end > 0x02000000ull ||
         !m_memory->contains(channel.m_madr, static_cast<size_t>(bytes))))
    {
        return false;
    }

    if (copy_data)
    {
        for (uint32_t i = 0; i < qwc; ++i)
        {
            const uint32_t spr_address =
                ee_memory::k_scratchpad_base +
                ((channel.m_sadr + i * 16u) & 0x3ff0u);
            const uint32_t ram_address = channel.m_madr + i * 16u;
            s_ee_u128 quad{};
            m_memory->read128(spr_address, &quad);
            m_memory->write128(ram_address, &quad);
        }
    }

    channel.m_sadr =
        (channel.m_sadr + static_cast<uint32_t>(bytes)) & 0x3ff0u;
    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_qwc = 0u;
    return true;
}

bool c_ee_dmac::walk_from_spr_chain(
    s_spr_channel& channel, bool copy_data)
{
    if (m_memory == nullptr ||
        ((channel.m_chcr >> 2u) & 3u) != 1u)
    {
        return false;
    }

    // ps2-web-recomp runs a preloaded QWC segment first, if present,
    // using the existing CHCR tag ID/IRQ bits to determine termination.
    const bool tie = (channel.m_chcr & 0x80u) != 0u;
    bool done = false;
    if (channel.m_qwc != 0u)
    {
        done = ((channel.m_chcr >> 28u) & 7u) == 7u ||
            (tie && (channel.m_chcr & 0x80000000u) != 0u);
        if (!transfer_from_spr_span(
                channel, channel.m_qwc, copy_data))
        {
            return false;
        }
    }

    // The reference has a very large loop guard. This conservative
    // implementation caps the number of tags at 4096 and refuses to
    // complete a nonterminating chain. A preflight pass guarantees
    // invalid later tags cannot cause partial writes to EE RAM.
    constexpr uint32_t k_max_chain_tags = 4096u;
    for (uint32_t i = 0; !done && i < k_max_chain_tags; ++i)
    {
        const uint32_t tag_address =
            ee_memory::k_scratchpad_base + (channel.m_sadr & 0x3ff0u);
        const uint64_t tag = m_memory->read64(tag_address);
        channel.m_sadr = (channel.m_sadr + 16u) & 0x3ff0u;

        // DMA tag bits 16..31 update the CHCR upper half; the high
        // 32-bit tag word sets the RAM destination address.
        channel.m_chcr =
            (channel.m_chcr & 0x0000ffffu) |
            (static_cast<uint32_t>(tag) & 0xffff0000u);
        const uint32_t qwc = static_cast<uint32_t>(tag) & 0xffffu;
        const uint32_t tag_id =
            (static_cast<uint32_t>(tag) >> 28u) & 7u;
        channel.m_madr = static_cast<uint32_t>(tag >> 32u);

        if (!transfer_from_spr_span(channel, qwc, copy_data))
        {
            return false;
        }

        done = tag_id == 7u ||
            (tie && (tag & 0x80000000ull) != 0u);
    }

    return done;
}

bool c_ee_dmac::transfer_from_spr_chain()
{
    if (m_memory == nullptr)
    {
        return false;
    }

    // First pass: check every destination and verify termination without
    // modifying guest memory or visible channel registers.
    s_spr_channel candidate = m_spr_channels[0];
    if (!walk_from_spr_chain(candidate, false))
    {
        return false;
    }

    // Only validated chains are copied. The source tags/data live in
    // SPR and destination addresses are restricted to main EE RAM, so
    // the copy cannot modify tags consumed by the second pass.
    candidate = m_spr_channels[0];
    if (!walk_from_spr_chain(candidate, true))
    {
        return false;
    }

    candidate.m_chcr &= ~k_chcr_str;
    m_spr_channels[0] = candidate;
    return raise(8u);
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
            const uint32_t mode = (channel->m_chcr >> 2u) & 3u;
            if (channel_index == 8u && mode == 1u)
            {
                (void)transfer_from_spr_chain();
            }
            else if (mode == 2u && ((m_sqwc >> 16u) & 0xffu) != 0u)
            {
                (void)transfer_spr_interleave(channel_index);
            }
            else
            {
                (void)transfer_spr_normal(channel_index);
            }
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
    else if (address == k_sqwc_address)
    {
        word = controller->m_sqwc;
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
    if (address == k_sqwc_address)
    {
        controller->m_sqwc = word;
        return true;
    }

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
