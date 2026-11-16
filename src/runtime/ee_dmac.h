#pragma once

#include "ee_runtime.h"
#include "ee_memory.h"

#include <cstdint>

namespace ee
{
// EE DMA registers, normal/interleave SPR DMA, bounded SPR chains,
// and normal DMA for VIF0/VIF1/GIF through an optional host consumer.
// SIF0 channel 5 and SIF1 channel 6 registers are readable/writable,
// but genuine EE/IOP DMA transport is not connected yet.
// Includes VIF1 reverse GS image downloads (CHCR.DIR=0).
// Reference: ps2-web-recomp/runtime/src/dmac.cpp and hw.cpp.
// Transfers execute synchronously, prevalidating physical RAM ranges.
// MFIFO, general device emulation, chain DMA for channels 0..2,
// arbitration and cycle-accurate completion remain unimplemented.
class c_ee_dmac
{
public:
    static constexpr uint32_t k_vif0_base = 0x10008000u; // channel 0
    static constexpr uint32_t k_vif1_base = 0x10009000u; // channel 1
    static constexpr uint32_t k_gif_base = 0x1000a000u; // channel 2
    static constexpr uint32_t k_sif0_base = 0x1000c000u; // channel 5, IOP -> EE
    static constexpr uint32_t k_sif1_base = 0x1000c400u; // channel 6, EE -> IOP
    static constexpr uint32_t k_stat_address = 0x1000e010u;
    static constexpr uint32_t k_sqwc_address = 0x1000e030u;
    static constexpr uint32_t k_channel_count = 10u;
    static constexpr uint32_t k_to_spr_base = 0x1000d400u; // channel 9
    static constexpr uint32_t k_from_spr_base = 0x1000d000u; // channel 8
    static constexpr uint32_t k_chcr_str = 0x100u;

    void bind_cpu(s_ee_cpu_state* cpu);
    void bind_memory(ee_memory* memory);

    // A host-owned, synchronous data consumer. channel is 0=VIF0,
    // 1=VIF1 (RAM-to-device), 2=GIF PATH3. data is a contiguous,
    // little-endian array of 16*qwc bytes valid only during the call.
    // Return true only after consuming the ENTIRE transfer. A false
    // return means the host rejected it without side effects; the DMA
    // controller leaves CHCR.STR, MADR and QWC unchanged.
    using t_device_transfer_callback = bool (*)(
        void* user, uint32_t channel, const uint8_t* data, uint32_t qwc
    );
    void bind_device_transfer(void* user, t_device_transfer_callback callback);

    // VIF1 channel 1, CHCR.DIR=0: device-to-EE-RAM DMA.
    // The producer fills all 16*qwc bytes or rejects the entire
    // transfer without changing its internal state. On rejection,
    // DMAC leaves EE RAM, MADR, QWC, STR and D_STAT unchanged.
    using t_device_download_callback = bool (*)(
        void* user, uint32_t channel, uint8_t* data, uint32_t qwc
    );
    void bind_device_download(
        void* user, t_device_download_callback callback
    );

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
        uint32_t m_asr[2]{};
    };

    void sync_interrupt_line();
    bool read_spr_channel(uint32_t address, uint32_t* value) const;
    bool write_spr_channel(uint32_t address, uint32_t value);
    bool transfer_device_normal(uint32_t channel);
    bool transfer_spr_normal(uint32_t channel);
    bool transfer_spr_interleave(uint32_t channel);
    bool transfer_from_spr_chain();
    bool transfer_to_spr_chain();
    bool walk_to_spr_chain(s_spr_channel& channel, bool copy_data);
    bool transfer_to_spr_span(s_spr_channel& channel, uint32_t qwc, bool copy_data);
    bool walk_from_spr_chain(s_spr_channel& channel, bool copy_data);
    bool transfer_from_spr_span(s_spr_channel& channel, uint32_t qwc, bool copy_data);

    s_ee_cpu_state* m_cpu = nullptr;
    ee_memory* m_memory = nullptr;
    void* m_device_user = nullptr;
    t_device_transfer_callback m_device_transfer = nullptr;
    void* m_download_user = nullptr;
    t_device_download_callback m_device_download = nullptr;
    uint32_t m_status = 0;
    uint32_t m_sqwc = 0;
    s_spr_channel m_device_channels[3]{}; // VIF0, VIF1, GIF PATH3
    s_spr_channel m_sif0_channel{}; // SIF0, no IOP producer yet
    s_spr_channel m_sif1_channel{}; // SIF1, no IOP receiver yet
    s_spr_channel m_spr_channels[2]{}; // fromSPR channel 8, toSPR channel 9
};
} // namespace ee
