#include "ee_scanout_clock.h"
#include "ee_intc.h"
#include "ee_vsync_flags.h"
#include "ee_memory.h"

#include <limits>

namespace ee
{
void c_ee_scanout_clock::queue_next_field()
{
    const uint64_t whole = m_field_numerator / m_field_denominator;
    const uint32_t fractional = static_cast<uint32_t>(
        m_field_numerator % m_field_denominator
    );
    m_next_field_cycle += whole;
    m_fraction += fractional;
    if (m_fraction >= m_field_denominator)
    {
        ++m_next_field_cycle;
        m_fraction -= m_field_denominator;
    }
}

bool c_ee_scanout_clock::configure(
    uint32_t interlace, uint32_t mode, uint32_t ffmd,
    uint64_t current_ee_cycle
)
{
    if (interlace > 1u || ffmd > 1u ||
        (mode != 0x02u && mode != 0x03u) ||
        current_ee_cycle > std::numeric_limits<uint64_t>::max() -
            2u * k_ee_clock_hz)
    {
        return false;
    }

    const uint32_t denominator = mode == 0x02u ? 60000u : 50u;
    const uint64_t numerator = mode == 0x02u ?
        k_ee_clock_hz * 1001ull : k_ee_clock_hz;

    m_enabled = true;
    m_interlaced = interlace != 0u;
    m_last_ee_cycle = current_ee_cycle;
    m_next_field_cycle = current_ee_cycle;
    m_field_numerator = numerator;
    m_field_denominator = denominator;
    m_fraction = 0u;
    m_csr = 0u;
    m_fields_started = 0u;
    queue_next_field();
    return true;
}

bool c_ee_scanout_clock::advance_to(
    uint64_t ee_cycle, c_ee_intc* intc,
    c_ee_vsync_flags* flags, ee_memory* memory
)
{
    if (!m_enabled || ee_cycle < m_last_ee_cycle ||
        intc == nullptr || flags == nullptr || memory == nullptr)
    {
        return false;
    }

    // Reject impractically huge jumps without producing a partial series
    // of VBlank events. This protects against malformed cycle timestamps.
    constexpr uint64_t k_max_fields_per_advance = 1024ull;
    if (ee_cycle >= m_next_field_cycle &&
        (ee_cycle - m_next_field_cycle) /
            (m_field_numerator / m_field_denominator) >= k_max_fields_per_advance)
    {
        return false;
    }

    while (ee_cycle >= m_next_field_cycle)
    {
        uint64_t next_csr = m_csr | k_csr_vsint;
        if (m_interlaced)
        {
            next_csr ^= k_csr_field;
        }
        // Validate/deliver before changing our latch or INTC state. The
        // one-shot registered flag may be null, in which case no RAM write.
        if (!flags->deliver_actual_vsync(memory, next_csr))
        {
            return false;
        }
        if (!intc->raise(k_intc_vblank_start))
        {
            return false;
        }
        m_csr = next_csr;
        ++m_fields_started;
        queue_next_field();
    }
    m_last_ee_cycle = ee_cycle;
    return true;
}

void c_ee_scanout_clock::acknowledge_csr(uint64_t write_one_to_clear)
{
    m_csr &= ~(write_one_to_clear & 0x1full);
}

bool c_ee_scanout_clock::enabled() const
{
    return m_enabled;
}

uint64_t c_ee_scanout_clock::next_field_cycle() const
{
    return m_next_field_cycle;
}

uint64_t c_ee_scanout_clock::last_ee_cycle() const
{
    return m_last_ee_cycle;
}

uint64_t c_ee_scanout_clock::csr() const
{
    return m_csr;
}

uint64_t c_ee_scanout_clock::fields_started() const
{
    return m_fields_started;
}
} // namespace ee
