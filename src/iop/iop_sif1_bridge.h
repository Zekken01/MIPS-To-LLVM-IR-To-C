#pragma once

#include "iop_cpu.h"
#include "ee_dmac.h"
#include "ee_memory.h"

#include <cstdint>

namespace iop
{
class c_sifcmd_bridge;
class c_sif_rpc_bridge;
// An unhandled guest SIF1 descriptor is a distinct validation failure.
// The record is diagnostic only; no IOP state or DMA is committed.
enum class e_sif1_dma_failure
{
    none,
    invalid_list,
    invalid_size_or_attributes,
    invalid_source,
    invalid_destination,
    unsupported_iop_reboot,
    command_buffer_overlap,
    duplicate_rpc_payload,
    unsupported_rpc_packet,
    unsupported_sifcmd_packet
};

struct s_sif1_dma_diagnostic
{
    e_sif1_dma_failure m_failure = e_sif1_dma_failure::none;
    uint32_t m_descriptor_index = 0xffffffffu;
    uint32_t m_source = 0u;
    uint32_t m_destination = 0u;
    uint32_t m_size = 0u;
    uint32_t m_attributes = 0u;
    uint32_t m_packet_size = 0u;
    uint32_t m_packet_cid = 0u;
};

// Guest EE sceSifSetDma (syscall 0x77) to IOP RAM transport.
//
// This is a synchronous, bounded transport foundation, not an IOP BIOS,
// SIFCMD dispatcher or cycle-accurate DMA scheduler. A completed DMA may
// cause an EE DMAC channel-6 event, but never publishes IOP readiness.
class c_sif1_bridge
{
public:
    c_sif1_bridge(
        ee::ee_memory& ee_ram, c_cpu& iop_cpu, ee::c_ee_dmac& dmac,
        c_sifcmd_bridge* sifcmd = nullptr,
        c_sif_rpc_bridge* rpc = nullptr
    );

    // Returns false on unsupported destinations, flags, or unmapped ranges.
    // Validate and stage the entire descriptor list before any IOP write.
    bool set_dma(
        uint32_t list, uint32_t count, uint32_t* dma_id,
        s_sif1_dma_diagnostic* diagnostic = nullptr
    );

    static const char* failure_name(e_sif1_dma_failure failure);

    // Only IDs produced by a successful transfer have a status. Until a
    // cycle-based scheduler exists, transfers finish synchronously (-1).
    bool dma_stat(uint32_t dma_id, int32_t* result) const;

private:
    ee::ee_memory& m_ee_ram;
    c_cpu& m_iop_cpu;
    ee::c_ee_dmac& m_dmac;
    c_sifcmd_bridge* m_sifcmd = nullptr;
    c_sif_rpc_bridge* m_rpc = nullptr;
    uint32_t m_next_dma_id = 1u;
};
} // namespace iop
