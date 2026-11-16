#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
    // OpenPS2BIOS emits guest TTY bytes to this emulator-facing port.
    // It is a write-only diagnostic extension, not IOP boot readiness.
    static constexpr uint32_t k_debug_console = 0x1f802080u;
    static constexpr size_t k_debug_console_capacity = 4096u;
    // CDVD byte register: 0x00 means no inserted disc.
    // Standalone ROM0 boot has no attached media. A media backend
    // must eventually replace this default with detected disc type.
    // Timer 5 is the IOP kernel's 32-bit periodic clock (IRQ 16).
    static constexpr uint32_t k_timer5_count = 0x1f8014a0u;
    static constexpr uint32_t k_timer5_mode = 0x1f8014a4u;
    static constexpr uint32_t k_timer5_target = 0x1f8014a8u;
    static constexpr uint32_t k_timer5_irq = 16u;
    static constexpr uint32_t k_cdvd_disc_type = 0x1f40200fu;
    static constexpr uint8_t k_cdvd_no_disc = 0x00u;

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
    // Advance IOP timer clock by completed guest execution cycles.
    // The standalone R3000 runtime currently counts one cycle per instruction.
    void advance_cycles(uint32_t cycles);
    const std::string& debug_console() const;
    bool debug_console_truncated() const;

private:
    bool scratchpad_range(uint32_t address, uint32_t width) const;

    uint8_t m_scratchpad[k_scratchpad_size]{};
    uint32_t m_i_stat = 0u;
    uint32_t m_i_mask = 0u;
    uint32_t m_i_ctrl = 1u;
    uint32_t m_cache_control = 0u;
    uint32_t m_timer5_count = 0u;
    uint32_t m_timer5_mode = 0u;
    uint32_t m_timer5_target = 0u;
    uint32_t m_timer5_remainder = 0u;
    bool m_timer5_irq_armed = true;
    std::string m_debug_console;
    bool m_debug_console_truncated = false;
};
} // namespace iop
