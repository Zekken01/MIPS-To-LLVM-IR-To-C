#include "ee_dmac.h"
#include "ee_exceptions.h"

#include <vector>

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

void c_ee_dmac::bind_device_transfer(
    void* user, t_device_transfer_callback callback)
{
    m_device_user = user;
    m_device_transfer = callback;
}

void c_ee_dmac::bind_device_download(
    void* user, t_device_download_callback callback)
{
    m_download_user = user;
    m_device_download = callback;
}

void c_ee_dmac::reset()
{
    m_status = 0u;
    m_sqwc = 0u;
    for (s_spr_channel& channel : m_device_channels)
    {
        channel = {};
    }
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
    bool device_channel = false;
    if (address >= k_vif0_base && address < k_vif0_base + 0x100u)
    {
        channel = &m_device_channels[0];
        offset = address - k_vif0_base;
        device_channel = true;
    }
    else if (address >= k_vif1_base && address < k_vif1_base + 0x100u)
    {
        channel = &m_device_channels[1];
        offset = address - k_vif1_base;
        device_channel = true;
    }
    else if (address >= k_gif_base && address < k_gif_base + 0x100u)
    {
        channel = &m_device_channels[2];
        offset = address - k_gif_base;
        device_channel = true;
    }
    else if (address >= k_from_spr_base && address < k_from_spr_base + 0x100u)
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
    case 0x40u: *value = channel->m_asr[0]; return true;
    case 0x50u: *value = channel->m_asr[1]; return true;
    case 0x80u:
    {
        if (device_channel)
        {
            return false;
        }
        *value = channel->m_sadr;
        return true;
    }
    default: return false;
    }
}

bool c_ee_dmac::transfer_device_normal(uint32_t channel_index)
{
    if (channel_index > 2u || m_memory == nullptr)
    {
        return false;
    }

    s_spr_channel& channel = m_device_channels[channel_index];
    // Normal mode is supported for VIF0/VIF1/GIF. VIF1 CHCR.DIR=0
    // is a device-to-RAM image download, not a forward VIF stream.
    if (((channel.m_chcr >> 2u) & 3u) != 0u)
    {
        return false;
    }

    const size_t bytes = static_cast<size_t>(channel.m_qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;
    if ((channel.m_madr & 0x0fu) != 0u ||
        channel.m_madr >= 0x02000000u ||
        end > 0x02000000ull ||
        !m_memory->contains(channel.m_madr, bytes))
    {
        return false;
    }

    // A zero-QWC DMA completes without invoking either device.
    if (channel.m_qwc != 0u)
    {
        std::vector<uint8_t> data(bytes);
        const bool reverse_vif1 =
            channel_index == 1u && (channel.m_chcr & 1u) == 0u;
        if (reverse_vif1)
        {
            if (m_device_download == nullptr ||
                !m_device_download(
                    m_download_user, channel_index,
                    data.data(), channel.m_qwc))
            {
                return false;
            }

            // All destination RAM spans were prevalidated. Do not
            // modify EE RAM unless the full device read succeeded.
            for (uint32_t i = 0; i < channel.m_qwc; ++i)
            {
                s_ee_u128 quad{};
                for (uint32_t b = 0u; b < 8u; ++b)
                {
                    quad.m_lo |= static_cast<uint64_t>(
                        data[static_cast<size_t>(i) * 16u + b]
                    ) << (b * 8u);
                    quad.m_hi |= static_cast<uint64_t>(
                        data[static_cast<size_t>(i) * 16u + 8u + b]
                    ) << (b * 8u);
                }
                m_memory->write128(
                    channel.m_madr + i * 16u, &quad
                );
            }
        }
        else
        {
            if (m_device_transfer == nullptr)
            {
                return false;
            }

            // Deliver the COMPLETE transfer as one contiguous
            // little-endian guest stream. A refusing consumer leaves
            // DMA registers and device state unchanged.
            for (uint32_t i = 0; i < channel.m_qwc; ++i)
            {
                s_ee_u128 quad{};
                m_memory->read128(channel.m_madr + i * 16u, &quad);
                for (uint32_t b = 0; b < 8u; ++b)
                {
                    data[static_cast<size_t>(i) * 16u + b] =
                        static_cast<uint8_t>(quad.m_lo >> (b * 8u));
                    data[static_cast<size_t>(i) * 16u + 8u + b] =
                        static_cast<uint8_t>(quad.m_hi >> (b * 8u));
                }
            }
            if (!m_device_transfer(
                    m_device_user, channel_index,
                    data.data(), channel.m_qwc))
            {
                return false;
            }
        }
    }

    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_qwc = 0u;
    channel.m_chcr &= ~k_chcr_str;
    return raise(channel_index);
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

bool c_ee_dmac::transfer_to_spr_span(
    s_spr_channel& channel, uint32_t qwc, bool copy_data)
{
    // Chain sources for channel 9 are physical EE RAM, never MMIO,
    // guest KSEG pointers, or VU memory. Validate a whole span before
    // copying, mirroring the strict normal/interleave RAM policy.
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
            const uint32_t ram_address = channel.m_madr + i * 16u;
            const uint32_t spr_address = ee_memory::k_scratchpad_base +
                ((channel.m_sadr + i * 16u) & 0x3ff0u);
            s_ee_u128 quad{};
            m_memory->read128(ram_address, &quad);
            m_memory->write128(spr_address, &quad);
        }
    }

    channel.m_sadr =
        (channel.m_sadr + static_cast<uint32_t>(bytes)) & 0x3ff0u;
    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_qwc = 0u;
    return true;
}

bool c_ee_dmac::walk_to_spr_chain(
    s_spr_channel& channel, bool copy_data)
{
    if (m_memory == nullptr ||
        ((channel.m_chcr >> 2u) & 3u) != 1u)
    {
        return false;
    }

    const bool tie = (channel.m_chcr & 0x80u) != 0u;
    bool end = false;
    if (channel.m_qwc != 0u)
    {
        const uint32_t id = (channel.m_chcr >> 28u) & 7u;
        end = id == 0u || id == 7u ||
            (tie && (channel.m_chcr & 0x80000000u) != 0u);
        if (!transfer_to_spr_span(channel, channel.m_qwc, copy_data))
        {
            return false;
        }
    }

    // The reference implementation walks at most 1,000,000 tags.
    // Bound this experimental runtime to 4,096 to prevent untrusted,
    // cyclic or malformed DMA chains from stalling the native host.
    constexpr uint32_t k_max_chain_tags = 4096u;
    for (uint32_t i = 0; !end && i < k_max_chain_tags; ++i)
    {
        const uint64_t tag_address = channel.m_tadr;
        if ((tag_address & 0x0fu) != 0u ||
            tag_address >= 0x02000000ull ||
            tag_address + 16u > 0x02000000ull ||
            !m_memory->contains(static_cast<uint32_t>(tag_address), 16u))
        {
            return false;
        }

        const uint64_t tag = m_memory->read64(channel.m_tadr);
        const uint32_t low = static_cast<uint32_t>(tag);
        const uint32_t qwc = low & 0xffffu;
        const uint32_t id = (low >> 28u) & 7u;
        const bool irq = (low & 0x80000000u) != 0u;
        // The reference masks off low address bits but preserves bit31.
        const uint32_t addr =
            static_cast<uint32_t>(tag >> 32u) & 0xfffffff0u;
        const uint64_t after_tag = tag_address + 16u;
        const uint64_t after_data =
            after_tag + static_cast<uint64_t>(qwc) * 16u;
        channel.m_chcr = (channel.m_chcr & 0x0000ffffu) |
            (low & 0xffff0000u);
        uint32_t data_address = static_cast<uint32_t>(after_tag);

        switch (id)
        {
        case 0u: // REFE: pointed-to payload, then terminate
            data_address = addr;
            channel.m_tadr = static_cast<uint32_t>(after_tag);
            end = true;
            break;
        case 1u: // CNT: inline data, then next sequential tag
            if (after_data > 0xffffffffull)
            {
                return false;
            }
            channel.m_tadr = static_cast<uint32_t>(after_data);
            break;
        case 2u: // NEXT: inline data, then jump to the tag address
            channel.m_tadr = addr;
            break;
        case 3u: // REF: pointed-to payload, continue sequential tags
        case 4u: // REFS: equivalent source selection without stall events
            data_address = addr;
            channel.m_tadr = static_cast<uint32_t>(after_tag);
            break;
        case 5u: // CALL: inline payload and push next sequential tag
        {
            const uint32_t asp = (channel.m_chcr >> 4u) & 3u;
            if (after_data > 0xffffffffull)
            {
                return false;
            }
            if (asp < 2u)
            {
                channel.m_asr[asp] = static_cast<uint32_t>(after_data);
            }
            const uint32_t new_asp = asp < 2u ? asp + 1u : 2u;
            channel.m_chcr = (channel.m_chcr & ~0x30u) |
                (new_asp << 4u);
            channel.m_tadr = addr;
            break;
        }
        case 6u: // RET: return to most recent saved tag, or terminate
        {
            const uint32_t asp = (channel.m_chcr >> 4u) & 3u;
            if (asp != 0u && asp <= 2u)
            {
                channel.m_tadr = channel.m_asr[asp - 1u];
                channel.m_chcr = (channel.m_chcr & ~0x30u) |
                    ((asp - 1u) << 4u);
            }
            else if (asp == 0u)
            {
                end = true;
            }
            else
            {
                return false;
            }
            break;
        }
        case 7u: // END: inline data, terminate
            end = true;
            break;
        default:
            return false;
        }

        channel.m_madr = data_address;
        if (!transfer_to_spr_span(channel, qwc, copy_data))
        {
            return false;
        }

        if (tie && irq)
        {
            end = true;
        }
    }

    return end;
}

bool c_ee_dmac::transfer_to_spr_chain()
{
    if (m_memory == nullptr)
    {
        return false;
    }

    // Read-only dry run first: all tag/control flow and RAM spans must
    // be valid before the first scratchpad write. DMA source RAM cannot
    // be changed by writing SPR, so the second pass follows the same tags.
    s_spr_channel candidate = m_spr_channels[1];
    if (!walk_to_spr_chain(candidate, false))
    {
        return false;
    }

    candidate = m_spr_channels[1];
    if (!walk_to_spr_chain(candidate, true))
    {
        return false;
    }

    candidate.m_chcr &= ~k_chcr_str;
    m_spr_channels[1] = candidate;
    return raise(9u);
}

bool c_ee_dmac::write_spr_channel(uint32_t address, uint32_t value)
{
    s_spr_channel* channel = nullptr;
    uint32_t channel_index = 0;
    uint32_t offset = 0;
    bool device_channel = false;

    if (address >= k_vif0_base && address < k_vif0_base + 0x100u)
    {
        channel = &m_device_channels[0];
        channel_index = 0u;
        offset = address - k_vif0_base;
        device_channel = true;
    }
    else if (address >= k_vif1_base && address < k_vif1_base + 0x100u)
    {
        channel = &m_device_channels[1];
        channel_index = 1u;
        offset = address - k_vif1_base;
        device_channel = true;
    }
    else if (address >= k_gif_base && address < k_gif_base + 0x100u)
    {
        channel = &m_device_channels[2];
        channel_index = 2u;
        offset = address - k_gif_base;
        device_channel = true;
    }
    else if (address >= k_from_spr_base && address < k_from_spr_base + 0x100u)
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
            if (device_channel)
            {
                (void)transfer_device_normal(channel_index);
            }
            else if (channel_index == 8u && mode == 1u)
            {
                (void)transfer_from_spr_chain();
            }
            else if (channel_index == 9u && mode == 1u)
            {
                (void)transfer_to_spr_chain();
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
    case 0x40u: channel->m_asr[0] = value; return true;
    case 0x50u: channel->m_asr[1] = value; return true;
    case 0x80u:
    {
        if (device_channel)
        {
            return false;
        }
        channel->m_sadr = value & 0x3ff0u;
        return true;
    }
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
