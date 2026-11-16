#pragma once

#include <cstddef>
#include <cstdint>

namespace iop
{
// The first guest-visible IOP hardware registers needed for ROM0
// exploration. This is NOT a complete IOP bus: unknown devices fail
// instead of reading zero or silently accepting writes.
class c_hw
{
public:
    static constexpr uint32_t k_scratchpad_base = 0x1f800000u;
    static constexpr uint32_t k_scratchpad_size = 1024u;
    static constexpr uint32_t k_i_stat = 0x1f801070u;
    static constexpr uint32_t k_i_mask = 0x1f801074u;
    static constexpr uint32_t k_i_ctrl = 0x1f801078u;
    static constexpr uint32_t k_cache_control = 0xfffe0130u;

    c_hw();
    void reset();

    // For the ordinary MMIO/scratchpad range, address is physical.
    // The cache-control register retains its privileged full address.
    // Reads may have side effects (I_CTRL clears on read).
    bool read(uint32_t address, uint32_t width, uint32_t* value);
    bool write(uint32_t address, uint32_t width, uint32_t value);

    // IRQ bookkeeping only. Delivery through R3000 COP0 exception
    // entry is a separate milestone; this does not synthesize it.
    bool raise_interrupt(uint32_t line);
    uint32_t pending_interrupts() const;
    uint32_t cache_control() const;

private:
    bool scratchpad_range(uint32_t address, uint32_t width) const;

    uint8_t m_scratchpad[k_scratchpad_size]{};
    uint32_t m_i_stat = 0u;
    uint32_t m_i_mask = 0u;
    uint32_t m_i_ctrl = 1u;
    uint32_t m_cache_control = 0u;
};
} // namespace iop
