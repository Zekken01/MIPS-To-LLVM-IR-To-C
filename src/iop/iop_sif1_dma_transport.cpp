#include "iop_sif1_dma_transport.h"

namespace iop
{
c_sif1_dma_transport::c_sif1_dma_transport(
    ee::ee_memory& ee_ram, ee::c_ee_dmac& ee_dmac,
    c_cpu& iop_cpu, c_hw& iop_hw
)
    : m_ee_ram(ee_ram), m_ee_dmac(ee_dmac),
      m_iop_cpu(iop_cpu), m_iop_hw(iop_hw)
{
    reset();
    m_ee_dmac.bind_sif1_start(this, &start_callback);
    m_iop_hw.bind_sif1_start(this, &start_callback);
}

c_sif1_dma_transport::~c_sif1_dma_transport()
{
    m_ee_dmac.bind_sif1_start(nullptr, nullptr);
    m_iop_hw.bind_sif1_start(nullptr, nullptr);
}

void c_sif1_dma_transport::reset()
{
    m_fifo.fill(0u);
    m_head = 0u;
    m_count = 0u;
    m_iop_destination = 0u;
    m_iop_words_remaining = 0u;
    m_iop_final_tag = false;
    m_in_pump = false;
    m_faulted = false;
    m_queued_bytes = 0u;
    m_delivered_bytes = 0u;
}

void c_sif1_dma_transport::start_callback(void* user)
{
    if (user != nullptr)
    {
        static_cast<c_sif1_dma_transport*>(user)->pump();
    }
}

bool c_sif1_dma_transport::read_ee_register(
    uint32_t offset, uint32_t* result)
{
    ee::s_ee_u128 word{};
    const bool valid = result != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &m_ee_dmac, ee::c_ee_dmac::k_sif1_base + offset,
            4u, &word);
    if (valid)
    {
        *result = static_cast<uint32_t>(word.m_lo);
    }
    return valid;
}

bool c_sif1_dma_transport::read_iop_register(
    uint32_t offset, uint32_t* result)
{
    return result != nullptr &&
        m_iop_hw.read(c_hw::k_sif1_dma_base + offset, 4u, result);
}

uint32_t c_sif1_dma_transport::fifo_peek(uint32_t offset) const
{
    return m_fifo[(m_head + offset) % k_fifo_words];
}

void c_sif1_dma_transport::fifo_write(uint32_t word)
{
    m_fifo[(m_head + m_count) % k_fifo_words] = word;
    ++m_count;
}

uint32_t c_sif1_dma_transport::fifo_read()
{
    const uint32_t word = m_fifo[m_head];
    m_head = (m_head + 1u) % k_fifo_words;
    --m_count;
    return word;
}

bool c_sif1_dma_transport::move_one_ee_quadword()
{
    uint32_t chcr = 0u;
    uint32_t madr = 0u;
    uint32_t qwc = 0u;
    if (!read_ee_register(0u, &chcr) ||
        !read_ee_register(0x10u, &madr) ||
        !read_ee_register(0x20u, &qwc))
    {
        m_faulted = true;
        return false;
    }

    if ((chcr & ee::c_ee_dmac::k_chcr_str) == 0u ||
        m_count > k_fifo_words - 4u)
    {
        return false;
    }

    // EE source tags are distinct from the 16-byte IOP destination
    // tags carried in the FIFO. Advance one source tag at a time,
    // without generating a completion on intermediate segments.
    const uint32_t mode = (chcr >> 2u) & 3u;
    if (mode == 1u && qwc == 0u)
    {
        if (!m_ee_dmac.prepare_sif1_chain_tag())
        {
            m_faulted = true;
            return false;
        }
        return true;
    }

    if ((chcr & 1u) == 0u || (mode != 0u && mode != 1u) ||
        qwc == 0u || (madr & 15u) != 0u ||
        madr >= 0x02000000u ||
        static_cast<uint64_t>(madr) + uint64_t(qwc) * 16u >
            0x02000000ull ||
        !m_ee_ram.contains(madr, static_cast<size_t>(qwc) * 16u))
    {
        m_faulted = true;
        return false;
    }

    // Every source qword is backed by guest EE memory. The RAM
    // snapshot is taken when the hardware pump reads it.
    ee::s_ee_u128 quad{};
    m_ee_ram.read128(madr, &quad);
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const uint64_t half = index < 2u ? quad.m_lo : quad.m_hi;
        fifo_write(static_cast<uint32_t>(
            half >> ((index & 1u) * 32u)));
    }
    m_queued_bytes += 16u;
    if (!m_ee_dmac.commit_sif1_qwords(1u))
    {
        m_faulted = true;
        return false;
    }
    return true;
}

bool c_sif1_dma_transport::consume_one_iop_word_or_tag()
{
    uint32_t chcr = 0u;
    if (!read_iop_register(8u, &chcr))
    {
        m_faulted = true;
        return false;
    }
    if ((chcr & 0x01000000u) == 0u)
    {
        return false;
    }

    if (m_iop_words_remaining == 0u && m_iop_final_tag)
    {
        m_iop_final_tag = false;
        if (!m_iop_hw.complete_dma_channel(10u))
        {
            m_faulted = true;
        }
        return !m_faulted;
    }

    if (m_iop_words_remaining == 0u)
    {
        if (m_count < 4u)
        {
            return false;
        }

        // The first two words of the IOP destination-chain header
        // contain MADR[23:0] and a word-aligned byte count. The upper
        // two words are EE tag data, not payload (PCSX2 Sif1.cpp).
        const uint32_t tag = fifo_peek(0u);
        const uint32_t count = fifo_peek(1u) & 0xffffcu;
        const uint32_t destination = tag & 0x00ffffffu;

        if (count == 0u || (destination & 3u) != 0u ||
            destination >= c_cpu::k_ram_size ||
            static_cast<uint64_t>(destination) + count >
                c_cpu::k_ram_size)
        {
            m_faulted = true;
            return false;
        }

        for (uint32_t index = 0u; index < 4u; ++index)
        {
            (void)fifo_read();
        }
        m_iop_destination = destination;
        m_iop_words_remaining = count / 4u;
        m_iop_final_tag = (tag & 0x80000000u) != 0u ||
                          (tag & 0x40000000u) != 0u;
        return true;
    }

    if (m_count == 0u)
    {
        return false;
    }

    if (!m_iop_cpu.write32(m_iop_destination, fifo_peek(0u)))
    {
        m_faulted = true;
        return false;
    }
    (void)fifo_read();
    m_iop_destination += 4u;
    --m_iop_words_remaining;
    m_delivered_bytes += 4u;
    if (!m_iop_hw.write(c_hw::k_sif1_dma_base, 4u,
                        m_iop_destination))
    {
        m_faulted = true;
        return false;
    }
    return true;
}

void c_sif1_dma_transport::pump()
{
    if (m_in_pump || m_faulted)
    {
        return;
    }
    m_in_pump = true;

    // Each iteration either moves real guest bytes, retires a tag or
    // completes a genuine DMA. A bounded loop handles 64 KiB chains
    // without an event-spin if one endpoint is not yet available.
    constexpr uint32_t k_max_steps = 1u << 18u;
    for (uint32_t index = 0u; index < k_max_steps; ++index)
    {
        bool progressed = false;
        if (consume_one_iop_word_or_tag())
        {
            progressed = true;
        }
        if (!m_faulted && move_one_ee_quadword())
        {
            progressed = true;
        }
        if (!progressed || m_faulted)
        {
            break;
        }
    }
    m_in_pump = false;
}

uint64_t c_sif1_dma_transport::queued_bytes() const
{
    return m_queued_bytes;
}

uint64_t c_sif1_dma_transport::delivered_bytes() const
{
    return m_delivered_bytes;
}

uint32_t c_sif1_dma_transport::fifo_words() const
{
    return m_count;
}

bool c_sif1_dma_transport::faulted() const
{
    return m_faulted;
}
} // namespace iop
