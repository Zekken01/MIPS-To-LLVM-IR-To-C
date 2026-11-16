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
    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }

    switch (address)
    {
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

    switch (address)
    {
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

const std::string& c_hw::debug_console() const
{
    return m_debug_console;
}

bool c_hw::debug_console_truncated() const
{
    return m_debug_console_truncated;
}
} // namespace iop
