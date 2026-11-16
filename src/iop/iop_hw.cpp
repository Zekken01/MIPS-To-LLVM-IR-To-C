#include "iop_hw.h"

#include <algorithm>

namespace iop
{
c_hw::c_hw()
{
    reset();
}

void c_hw::reset()
{
    std::fill(
        m_scratchpad,
        m_scratchpad + k_scratchpad_size,
        static_cast<uint8_t>(0u)
    );
    m_i_stat = 0u;
    m_i_mask = 0u;
    m_i_ctrl = 1u;
    m_cache_control = 0u;
    m_timer5_count = 0u;
    m_timer5_mode = 0u;
    m_timer5_target = 0u;
    m_timer5_remainder = 0u;
    m_timer5_irq_armed = true;
    m_dma_pcr = 0u;
    m_dma_pcr2 = 0u;
    m_dma_global_enable = 0u;
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            m_sif_dma[channel][reg] = 0u;
        }
    }
    m_dma_icr = 0u;
    m_dma_icr2 = 0u;
    m_debug_console.clear();
    m_debug_console_truncated = false;
}

bool c_hw::scratchpad_range(
    uint32_t address, uint32_t width
) const
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }
    return address >= k_scratchpad_base &&
           static_cast<uint64_t>(address - k_scratchpad_base) +
               width <= k_scratchpad_size &&
           (address & (width - 1u)) == 0u;
}

bool c_hw::sif_dma_register(uint32_t address, uint32_t* channel,
                            uint32_t* reg) const
{
    if (channel == nullptr || reg == nullptr)
    {
        return false;
    }
    if (address >= k_sif0_dma_base &&
        address < k_sif0_dma_base + 16u &&
        (address & 3u) == 0u)
    {
        *channel = 0u;
        *reg = (address - k_sif0_dma_base) / 4u;
        return true;
    }
    if (address >= k_sif1_dma_base &&
        address < k_sif1_dma_base + 16u &&
        (address & 3u) == 0u)
    {
        *channel = 1u;
        *reg = (address - k_sif1_dma_base) / 4u;
        return true;
    }
    return false;
}

bool c_hw::read(
    uint32_t address, uint32_t width, uint32_t* value
)
{
    if (value == nullptr ||
        (width != 1u && width != 2u && width != 4u))
    {
        return false;
    }

    if (scratchpad_range(address, width))
    {
        const uint32_t offset = address - k_scratchpad_base;
        uint32_t data = 0u;
        for (uint32_t i = 0u; i < width; ++i)
        {
            data |= static_cast<uint32_t>(
                m_scratchpad[offset + i]
            ) << (8u * i);
        }
        *value = data;
        return true;
    }
    // A physical CDVD drive with no media reports 0x00. The ROM0
    // diagnostic runner does not mount a disc. This deliberately does
    // not claim a successful CD read, device initialization, or SIF.
    if (address == k_cdvd_disc_type && width == 1u)
    {
        *value = k_cdvd_no_disc;
        return true;
    }
    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }

    uint32_t channel = 0u;
    uint32_t reg = 0u;
    if (sif_dma_register(address, &channel, &reg))
    {
        *value = m_sif_dma[channel][reg];
        return true;
    }

    switch (address)
    {
    case k_dma_pcr:
        *value = m_dma_pcr;
        return true;
    case k_dma_pcr2:
        *value = m_dma_pcr2;
        return true;
    case k_dma_global_enable:
        *value = m_dma_global_enable;
        return true;
    case k_dma_icr:
        *value = m_dma_icr;
        return true;
    case k_dma_icr2:
        *value = m_dma_icr2;
        return true;
    case k_timer5_count:
        *value = m_timer5_count;
        return true;
    case k_timer5_mode:
        *value = m_timer5_mode | (m_timer5_irq_armed ? 0x400u : 0u);
        // Counter target/overflow status bits are cleared on read.
        m_timer5_mode &= ~0x1800u;
        return true;
    case k_timer5_target:
        *value = m_timer5_target;
        return true;
    case k_i_stat:
        *value = m_i_stat;
        return true;
    case k_i_mask:
        *value = m_i_mask;
        return true;
    case k_i_ctrl:
        // On the IOP, reading I_CTRL disables the global interrupt
        // enable until the guest writes it back.
        *value = m_i_ctrl;
        m_i_ctrl = 0u;
        return true;
    case k_cache_control:
        // Register storage only. The emulator does not yet model
        // I-cache/D-cache isolation or instruction fetch coherence.
        *value = m_cache_control;
        return true;
    default:
        return false;
    }
}

bool c_hw::write(
    uint32_t address, uint32_t width, uint32_t value
)
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }
    if (scratchpad_range(address, width))
    {
        const uint32_t offset = address - k_scratchpad_base;
        for (uint32_t i = 0u; i < width; ++i)
        {
            m_scratchpad[offset + i] =
                static_cast<uint8_t>(value >> (8u * i));
        }
        return true;
    }
    // OpenPS2BIOS writes individual characters here. This address is
    // a debug-output convention in emulators, not a PS2 readiness flag.
    // Accept only the observed byte-store operation; fail other MMIO.
    if (address == k_debug_console && width == 1u)
    {
        if (m_debug_console.size() < k_debug_console_capacity)
        {
            m_debug_console.push_back(static_cast<char>(value & 0xffu));
        }
        else
        {
            m_debug_console_truncated = true;
        }
        return true;
    }
    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }

    uint32_t channel = 0u;
    uint32_t reg = 0u;
    if (sif_dma_register(address, &channel, &reg))
    {
        // CHCR.STR is guest-controlled when starting/stopping DMA, and
        // is NOT cleared merely because the register write succeeded.
        // The transfer backend will clear STR on real completion.
        m_sif_dma[channel][reg] = value;
        return true;
    }

    switch (address)
    {
    case k_dma_pcr:
        // Each DMA channel's priority and enable nibble is writable,
        // but programming it does not launch or complete a transfer.
        m_dma_pcr = value;
        return true;
    case k_dma_pcr2:
        m_dma_pcr2 = value;
        return true;
    case k_dma_global_enable:
        // This permits an explicitly started secondary DMA controller,
        // but never asserts a DMA completion or a SIF handshake.
        m_dma_global_enable = value;
        return true;
    case k_dma_icr:
        m_dma_icr = update_dma_icr(m_dma_icr, value);
        return true;
    case k_dma_icr2:
        m_dma_icr2 = update_dma_icr(m_dma_icr2, value);
        return true;
    case k_timer5_count:
        m_timer5_count = value;
        m_timer5_remainder = 0u;
        return true;
    case k_timer5_target:
        m_timer5_target = value;
        return true;
    case k_timer5_mode:
        // Only accept the supported counter-clock, target-reset/IRQ,
        // repeat, non-toggle mode, plus the disabled interrupt mode.
        // Unknown gate/source/overflow configurations must not run
        // with invented counter behaviour.
        if ((value & ~0x6058u) != 0u ||
            (value & 0x0058u) != 0x0058u)
        {
            // Also permit mode 0 to reset an unconfigured counter.
            if (value != 0u)
            {
                return false;
            }
        }
        m_timer5_mode = value & 0x6058u;
        m_timer5_count = 0u;
        m_timer5_remainder = 0u;
        m_timer5_irq_armed = true;
        return true;
    case k_i_stat:
        // Write-one keeps the IRQ; write-zero acknowledges it.
        m_i_stat &= value;
        return true;
    case k_i_mask:
        m_i_mask = value;
        return true;
    case k_i_ctrl:
        m_i_ctrl = value & 1u;
        return true;
    case k_cache_control:
        m_cache_control = value;
        return true;
    default:
        return false;
    }
}

void c_hw::assert_dma_irq(uint32_t& control, bool sif_channel)
{
    const bool force = (control & 0x00008000u) != 0u;
    const bool enabled = (control & 0x00800000u) != 0u;
    const bool channel_pending =
        (((control >> 8u) & control) & 0x007f0000u) != 0u;
    if (force || (enabled && channel_pending))
    {
        control |= 0x80000000u;
    }
    else
    {
        control &= ~0x80000000u;
    }
    if (sif_channel || (control & 0x80000000u) != 0u)
    {
        (void)raise_interrupt(k_dma_irq);
    }
}

uint32_t c_hw::update_dma_icr(uint32_t previous, uint32_t value)
{
    // Low 24 bits are writable control; pending bits 24..30 are
    // hardware flags acknowledged by write-one-to-clear. Bit 31
    // reflects the combined force/master/channel interrupt state.
    uint32_t result =
        (previous & 0x7f000000u) | (value & 0x00ffffffu);
    result &= ~(value & 0x7f000000u);
    assert_dma_irq(result, false);
    return result;
}

bool c_hw::complete_dma_channel(uint32_t channel)
{
    if (channel > 12u)
    {
        return false;
    }
    const bool secondary = channel >= 7u;
    const uint32_t index = secondary ? channel - 7u : channel;
    if (channel == 9u || channel == 10u)
    {
        // CHCR.STR clears only at a real completion supplied by the
        // DMA engine. Do not move bytes or synthesize a transfer here.
        m_sif_dma[channel - 9u][2u] &= ~0x01000000u;
    }
    uint32_t& control = secondary ? m_dma_icr2 : m_dma_icr;
    const bool sif_channel = secondary && (index == 2u || index == 3u);
    const uint32_t enable = uint32_t{1} << (16u + index);
    // PCSX2 models SIF0/SIF1 end/tag interrupts independently of
    // the per-channel mask; they do not fabricate any DMA completion.
    if ((control & enable) != 0u || sif_channel)
    {
        control |= uint32_t{1} << (24u + index);
        assert_dma_irq(control, sif_channel);
    }
    return true;
}

bool c_hw::raise_interrupt(uint32_t line)
{
    if (line >= 32u)
    {
        return false;
    }
    m_i_stat |= uint32_t{1} << line;
    return true;
}

uint32_t c_hw::pending_interrupts() const
{
    return m_i_ctrl != 0u ? (m_i_stat & m_i_mask) : 0u;
}

uint32_t c_hw::cache_control() const
{
    return m_cache_control;
}

void c_hw::advance_cycles(uint32_t cycles)
{
    if (cycles == 0u)
    {
        return;
    }
    // Timer 5 clock division, per IOP counter MODE[14:13].
    // The initial kernel tick uses /1, mode 0x58.
    const uint32_t shifts[4] = {1u, 8u, 16u, 256u};
    const uint32_t divider = shifts[(m_timer5_mode >> 13u) & 3u];
    const uint64_t accumulated =
        static_cast<uint64_t>(m_timer5_remainder) + cycles;
    const uint64_t elapsed = accumulated / divider;
    m_timer5_remainder = static_cast<uint32_t>(accumulated % divider);
    if (elapsed == 0u)
    {
        return;
    }

    const uint64_t count = static_cast<uint64_t>(m_timer5_count) + elapsed;
    if ((m_timer5_mode & 0x08u) != 0u &&
        m_timer5_target != 0u &&
        m_timer5_count < m_timer5_target &&
        count >= m_timer5_target)
    {
        const uint64_t over = count - m_timer5_target;
        m_timer5_count = static_cast<uint32_t>(
            over % m_timer5_target
        );
        m_timer5_mode |= 0x0800u;
        if ((m_timer5_mode & 0x10u) != 0u && m_timer5_irq_armed)
        {
            (void)raise_interrupt(k_timer5_irq);
            if ((m_timer5_mode & 0x40u) == 0u)
            {
                m_timer5_irq_armed = false;
            }
        }
    }
    else
    {
        // Unconfigured/free-running timer, or a counter past its
        // target: normal 32-bit rollover without an invented IRQ.
        m_timer5_count = static_cast<uint32_t>(count);
    }
}

const std::string& c_hw::debug_console() const
{
    return m_debug_console;
}

bool c_hw::debug_console_truncated() const
{
    return m_debug_console_truncated;
}
} // namespace iop
