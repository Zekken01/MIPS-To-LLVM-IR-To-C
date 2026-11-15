#pragma once

#include "ee_runtime.h"
#include "ee_memory.h"

#include <cstdint>

namespace ee
{
// EE DMA registers, normal/interleave SPR DMA, and bounded fromSPR chains.
// Reference: ps2-web-recomp/runtime/src/dmac.cpp and hw.cpp.
// Transfers execute synchronously, prevalidating all RAM destinations.
// MFIFO, toSPR chains, arbitration and cycle-accurate completion
// remain unimplemented.
class c_ee_dmac
{
public:
    static constexpr uint32_t k_stat_address = 0x1000e010u;
    static constexpr uint32_t k_sqwc_address = 0x1000e030u;
    static constexpr uint32_t k_channel_count = 10u;
    static constexpr uint32_t k_to_spr_base = 0x1000d400u; // channel 9
    static constexpr uint32_t k_from_spr_base = 0x1000d000u; // channel 8
    static constexpr uint32_t k_chcr_str = 0x100u;

    void bind_cpu(s_ee_cpu_state* cpu);
    void bind_memory(ee_memory* memory);
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
    struct s_spr_channel
    {
        uint32_t m_chcr = 0;
        uint32_t m_madr = 0;
        uint32_t m_qwc = 0;
        uint32_t m_tadr = 0;
        uint32_t m_sadr = 0;
    };

    void sync_interrupt_line();
    bool read_spr_channel(uint32_t address, uint32_t* value) const;
    bool write_spr_channel(uint32_t address, uint32_t value);
    bool transfer_spr_normal(uint32_t channel);
    bool transfer_spr_interleave(uint32_t channel);
    bool transfer_from_spr_chain();
    bool walk_from_spr_chain(s_spr_channel& channel, bool copy_data);
    bool transfer_from_spr_span(s_spr_channel& channel, uint32_t qwc, bool copy_data);

    s_ee_cpu_state* m_cpu = nullptr;
    ee_memory* m_memory = nullptr;
    uint32_t m_status = 0;
    uint32_t m_sqwc = 0;
    s_spr_channel m_spr_channels[2]{}; // [0] fromSPR channel 8, [1] toSPR channel 9
};
} // namespace ee
