#pragma once

#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
// EE DMA interrupt status and mask register (D_STAT). Data transfers,
// channel control, and event scheduling are deliberately not modelled here.
// Reference: ps2-web-recomp/runtime/src/hw.cpp::read32/write32/dmac_raise.
class c_ee_dmac
{
public:
    static constexpr uint32_t k_stat_address = 0x1000e010u;
    static constexpr uint32_t k_channel_count = 10u;

    void bind_cpu(s_ee_cpu_state* cpu);
    void reset();
    bool raise(uint32_t channel);
    bool acknowledge(uint32_t channel);

    uint32_t status() const;
    uint32_t pending() const;

    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value);
    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value);

private:
    void sync_interrupt_line();

    s_ee_cpu_state* m_cpu = nullptr;
    uint32_t m_status = 0;
};
} // namespace ee
