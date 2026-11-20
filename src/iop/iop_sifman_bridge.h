#pragma once

#include "iop_kernel.h"
#include "ee_sif.h"
#include "ee_dmac.h"
#include "ee_memory.h"

#include <cstdint>

namespace iop
{
// Explicit HLE of IOP SIFMAN initialization, synchronous DMA status and
// owned SIF mailbox operations. Stub installation never initializes
// the SIF or publishes readiness. Only guest execution mutates state.
// SIFMAN #32 callbacks execute original guest R3000 code after a real
// validated DMA transfer. The IOP cycle clock is not yet calibrated.        
//
// Stubs occupy the otherwise reserved low IOP kernel area, outside the
// allocator's 0x20000.. RAM region. Other HLE kernel services must use
// distinct reserved addresses and syscall identifiers.
class c_sifman_bridge
{
public:
    static constexpr uint32_t k_get_smflag_stub = 0x00018000u;
    static constexpr uint32_t k_set_dma_stub = 0x00018040u;
    // #32 cannot use k_extra_stub_base + 32 * 0x20: SIFCMD owns 0x18b00.
    static constexpr uint32_t k_set_dma_intr_stub = 0x00018060u;
    static constexpr uint32_t k_set_dma_syscall = 0x494f0007u;
    static constexpr uint32_t k_set_dma_intr_syscall = 0x494f0020u;
    static constexpr uint32_t k_set_smflag_stub = 0x00018020u;
    static constexpr uint32_t k_get_smflag_syscall = 0x494f0017u;
    static constexpr uint32_t k_set_smflag_syscall = 0x494f0018u;
    static constexpr uint32_t k_service_base = 0x494f0000u;
    // The separate CDVDMAN bridge occupies 0x18200..0x18680.
    static constexpr uint32_t k_extra_stub_base = 0x00018700u;

    c_sifman_bridge(
        c_kernel& kernel, ee::c_ee_sif& sif,
        ee::c_ee_dmac* dmac = nullptr,
        ee::ee_memory* ee_ram = nullptr
    );

    // Registers real guest-callable service-family stubs. Duplicate
    // library exports are not overwritten; only implemented callback
    // ordinals are exported.
    bool install();

    // Can be composed with other kernel syscall handlers. Unknown services
    // are NOT acknowledged or assigned invented return values.
    bool handle_syscall(c_kernel& kernel, uint32_t service);

private:
    bool initialize_dma2();
    bool configure_dma_chain();

    c_kernel& m_kernel;
    ee::c_ee_sif& m_sif;
    ee::c_ee_dmac* m_dmac = nullptr;
    ee::ee_memory* m_ee_ram = nullptr;
    uint32_t m_next_dma_id = 1u;
    uint32_t m_dma_success_log_count = 0u;
    bool m_dma2_initialized = false;
    bool m_initialized = false;
    bool m_dma_chain_configured = false;
};
} // namespace iop
