#pragma once

#include "ee_dmac.h"
#include "ee_memory.h"
#include "iop_cpu.h"
#include "iop_hw.h"

#include <array>
#include <cstdint>

namespace iop
{
// Guest-controlled IOP DMA9 -> EE DMA5 SIF0 transport.
//
// IOP DMA9 reads a 16-byte IOP source tag from its own RAM. The
// upper half of that tag, plus the following IOP tag header, forms
// a 16-byte EE destination tag on the FIFO (PCSX2 Sif0.cpp).
// This first milestone validates word/quadword-aligned firmware
// transfers and a bounded 128-word FIFO; unaligned padding, MFIFO,
// DMA stall control and cycle-accurate arbitration remain pending.
class c_sif0_dma_transport
{
public:
    static constexpr uint32_t k_fifo_words = 128u;

    c_sif0_dma_transport(ee::ee_memory& ee_ram, ee::c_ee_dmac& ee_dmac,
                         c_cpu& iop_cpu, c_hw& iop_hw);
    ~c_sif0_dma_transport();

    c_sif0_dma_transport(const c_sif0_dma_transport&) = delete;
    c_sif0_dma_transport& operator=(const c_sif0_dma_transport&) = delete;

    void reset();
    void pump();

    uint64_t queued_bytes() const;
    uint64_t delivered_bytes() const;
    uint32_t fifo_words() const;
    bool faulted() const;

private:
    static void start_callback(void* user);
    bool read_ee_register(uint32_t offset, uint32_t* value);
    bool read_iop_register(uint32_t offset, uint32_t* value);
    uint32_t fifo_peek(uint32_t offset) const;
    void fifo_write(uint32_t word);
    uint32_t fifo_read();
    bool produce_one_iop_quadword_or_tag();
    bool consume_one_ee_quadword_or_tag();

    ee::ee_memory& m_ee_ram;
    ee::c_ee_dmac& m_ee_dmac;
    c_cpu& m_iop_cpu;
    c_hw& m_iop_hw;

    std::array<uint32_t, k_fifo_words> m_fifo{};
    uint32_t m_head = 0u;
    uint32_t m_count = 0u;
    uint32_t m_iop_source = 0u;
    uint32_t m_iop_words_remaining = 0u;
    uint32_t m_iop_chain_tags = 0u;
    bool m_iop_final_tag = false;
    bool m_in_pump = false;
    bool m_faulted = false;
    uint64_t m_queued_bytes = 0u;
    uint64_t m_delivered_bytes = 0u;
};
} // namespace iop
