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
    using t_sif_mmio_read = bool (*)(
        void*, uint32_t, uint32_t, uint32_t*
    );
    using t_sif_mmio_write = bool (*)(
        void*, uint32_t, uint32_t, uint32_t
    );
    using t_sif1_start_callback = void (*)(void* user);
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
    static constexpr uint32_t k_dma_pcr = 0x1f8010f0u;
    static constexpr uint32_t k_dma_pcr2 = 0x1f801570u;
    // Secondary DMA global-enable control, used by the firmware's SIF.
    static constexpr uint32_t k_dma_global_enable = 0x1f801578u;
    // Secondary controller DMA9 (IOP -> EE SIF0) and
    // DMA10 (EE -> IOP SIF1) channel registers.
    static constexpr uint32_t k_sif0_dma_base = 0x1f801520u;
    static constexpr uint32_t k_sif1_dma_base = 0x1f801530u;
    // IOP-side SIF bus mirror of the EE's SBUS F200-F230 registers.
    static constexpr uint32_t k_sif_mscom = 0x1d000000u;
    static constexpr uint32_t k_sif_smcom = 0x1d000010u;
    static constexpr uint32_t k_sif_msflag = 0x1d000020u;
    static constexpr uint32_t k_sif_smflag = 0x1d000030u;
    static constexpr uint32_t k_dma_icr = 0x1f8010f4u;
    static constexpr uint32_t k_dma_icr2 = 0x1f801574u;
    static constexpr uint32_t k_dma_irq = 3u;
    static constexpr uint32_t k_cdvd_disc_type = 0x1f40200fu;
    static constexpr uint8_t k_cdvd_no_disc = 0x00u;

    c_hw();
    void reset();
    // Link to the EE's *real* SIF register model only when a live
    // EE runtime is attached. Local SIF state remains standalone.
    void bind_sif_mmio(void* user, t_sif_mmio_read read,
                       t_sif_mmio_write write);
    void bind_sif1_start(void* user, t_sif1_start_callback callback);

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
    // A future actual DMA channel completion may assert its flag.
    // Merely writing ICR enables must never complete any transfer.
    bool complete_dma_channel(uint32_t channel);
    // The EE side can announce a real request and acknowledge IOP
    // flag bits. Standalone ROM0 never invokes these operations.
    void ee_write_mscom(uint32_t value);
    void ee_set_msflag(uint32_t bits);
    void ee_clear_smflag(uint32_t bits);
    uint32_t sif_smcom() const;
    uint32_t sif_smflag() const;
    const std::string& debug_console() const;
    bool debug_console_truncated() const;

private:
    bool scratchpad_range(uint32_t address, uint32_t width) const;
    bool sif_dma_register(uint32_t address, uint32_t* channel,
                          uint32_t* reg) const;
    uint32_t update_dma_icr(uint32_t previous, uint32_t value);
    void assert_dma_irq(uint32_t& control, bool sif_channel);

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
    uint32_t m_dma_pcr = 0u;
    uint32_t m_dma_pcr2 = 0u;
    uint32_t m_dma_global_enable = 0u;
    // MADR, BCR, CHCR and TADR; transfers remain active until the
    // connected DMA backend explicitly completes them.
    uint32_t m_sif_dma[2][4]{};
    uint32_t m_sif_mscom = 0u;
    uint32_t m_sif_smcom = 0u;
    uint32_t m_sif_msflag = 0u;
    uint32_t m_sif_smflag = 0u;
    void* m_sif_mmio_user = nullptr;
    void* m_sif1_user = nullptr;
    t_sif1_start_callback m_sif1_start = nullptr;
    t_sif_mmio_read m_sif_mmio_read = nullptr;
    t_sif_mmio_write m_sif_mmio_write = nullptr;
    uint32_t m_dma_icr = 0u;
    uint32_t m_dma_icr2 = 0u;
    std::string m_debug_console;
    bool m_debug_console_truncated = false;
};
} // namespace iop
