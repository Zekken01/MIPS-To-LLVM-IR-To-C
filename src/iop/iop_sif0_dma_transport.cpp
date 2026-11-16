#include "iop_sif0_dma_transport.h"

#include <cstddef>

namespace iop
{
c_sif0_dma_transport::c_sif0_dma_transport(
    ee::ee_memory& ee_ram, ee::c_ee_dmac& ee_dmac,
    c_cpu& iop_cpu, c_hw& iop_hw
)
    : m_ee_ram(ee_ram), m_ee_dmac(ee_dmac),
      m_iop_cpu(iop_cpu), m_iop_hw(iop_hw)
{
    reset();
    m_ee_dmac.bind_sif0_start(this, &start_callback);
    m_iop_hw.bind_sif0_start(this, &start_callback);
}

c_sif0_dma_transport::~c_sif0_dma_transport()
{
    m_ee_dmac.bind_sif0_start(nullptr, nullptr);
    m_iop_hw.bind_sif0_start(nullptr, nullptr);
}

void c_sif0_dma_transport::reset()
{
    m_fifo.fill(0u);
    m_head = 0u;
    m_count = 0u;
    m_iop_source = 0u;
    m_iop_words_remaining = 0u;
    m_iop_chain_tags = 0u;
    m_iop_final_tag = false;
    m_in_pump = false;
    m_faulted = false;
    m_queued_bytes = 0u;
    m_delivered_bytes = 0u;
}

void c_sif0_dma_transport::start_callback(void* user)
{
    if (user != nullptr)
    {
        static_cast<c_sif0_dma_transport*>(user)->pump();
    }
}

bool c_sif0_dma_transport::read_ee_register(
    uint32_t offset, uint32_t* value)
{
    ee::s_ee_u128 word{};
    const bool ok = value != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &m_ee_dmac, ee::c_ee_dmac::k_sif0_base + offset,
            4u, &word);
    if (ok)
    {
        *value = static_cast<uint32_t>(word.m_lo);
    }
    return ok;
}

bool c_sif0_dma_transport::read_iop_register(
    uint32_t offset, uint32_t* value)
{
    return value != nullptr &&
        m_iop_hw.read(c_hw::k_sif0_dma_base + offset, 4u, value);
}

uint32_t c_sif0_dma_transport::fifo_peek(uint32_t offset) const
{
    return m_fifo[(m_head + offset) % k_fifo_words];
}

void c_sif0_dma_transport::fifo_write(uint32_t word)
{
    m_fifo[(m_head + m_count) % k_fifo_words] = word;
    ++m_count;
}

uint32_t c_sif0_dma_transport::fifo_read()
{
    const uint32_t word = m_fifo[m_head];
    m_head = (m_head + 1u) % k_fifo_words;
    --m_count;
    return word;
}

bool c_sif0_dma_transport::produce_one_iop_quadword_or_tag()
{
    uint32_t chcr = 0u;
    if (!read_iop_register(8u, &chcr))
    {
        m_faulted = true;
        return false;
    }
    if ((chcr & 0x01000000u) == 0u ||
        m_count > k_fifo_words - 4u)
    {
        return false;
    }

    if (m_iop_words_remaining == 0u && m_iop_final_tag)
    {
        m_iop_final_tag = false;
        m_iop_chain_tags = 0u;
        if (!m_iop_hw.complete_dma_channel(9u))
        {
            m_faulted = true;
        }
        return !m_faulted;
    }

    if (m_iop_words_remaining == 0u)
    {
        uint32_t tadr = 0u;
        if (!read_iop_register(12u, &tadr) ||
            (tadr & 15u) != 0u ||
            tadr >= c_cpu::k_ram_size ||
            static_cast<uint64_t>(tadr) + 24u >
                c_cpu::k_ram_size ||
            m_iop_chain_tags >= 4096u)
        {
            m_faulted = true;
            return false;
        }

        uint32_t tag[6]{};
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            if (!m_iop_cpu.read32(tadr + i * 4u, &tag[i]))
            {
                m_faulted = true;
                return false;
            }
        }

        const uint32_t src = tag[0u] & 0x00ffffffu;
        const uint32_t words = tag[1u] & 0x000fffffu;
        const uint32_t ee_qwc = tag[2u] & 0xffffu;
        const uint32_t ee_id = (tag[2u] >> 28u) & 7u;
        const uint32_t ee_dest = tag[3u] & ~15u;
        if (words == 0u || (words & 3u) != 0u ||
            words != ee_qwc * 4u ||
            (src & 3u) != 0u ||
            src >= c_cpu::k_ram_size ||
            static_cast<uint64_t>(src) + uint64_t(words) * 4u >
                c_cpu::k_ram_size ||
            (ee_id != 1u && ee_id != 4u && ee_id != 7u) ||
            ee_dest >= 0x02000000u ||
            static_cast<uint64_t>(ee_dest) +
                uint64_t(ee_qwc) * 16u > 0x02000000ull ||
            !m_ee_ram.contains(ee_dest,
                               static_cast<size_t>(ee_qwc) * 16u))
        {
            m_faulted = true;
            return false;
        }

        // The EE tag is the upper half of this IOP tag; the second
        // half of the 128-bit FIFO header comes from the next IOP tag.
        // This is hardware-authentic header transfer, not HLE RPC data.
        for (uint32_t i = 2u; i < 6u; ++i)
        {
            fifo_write(tag[i]);
        }
        if (!m_iop_hw.write(c_hw::k_sif0_dma_base + 12u,
                            4u, tadr + 16u) ||
            !m_iop_hw.write(c_hw::k_sif0_dma_base,
                            4u, src))
        {
            m_faulted = true;
            return false;
        }
        m_iop_source = src;
        m_iop_words_remaining = words;
        m_iop_final_tag = (tag[0u] & 0xc0000000u) != 0u;
        ++m_iop_chain_tags;
        m_queued_bytes += 16u;
        return true;
    }

    // IOP source RAM is checked for the full transfer before its tag
    // reaches the FIFO. Each quadword is read at the moment DMA runs.
    uint32_t data[4]{};
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!m_iop_cpu.read32(m_iop_source + i * 4u, &data[i]))
        {
            m_faulted = true;
            return false;
        }
    }
    for (uint32_t word : data)
    {
        fifo_write(word);
    }
    m_iop_source += 16u;
    m_iop_words_remaining -= 4u;
    if (!m_iop_hw.write(c_hw::k_sif0_dma_base,
                        4u, m_iop_source))
    {
        m_faulted = true;
        return false;
    }
    m_queued_bytes += 16u;
    return true;
}

bool c_sif0_dma_transport::consume_one_ee_quadword_or_tag()
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
        m_count < 4u)
    {
        return false;
    }

    if (((chcr >> 2u) & 3u) != 1u)
    {
        m_faulted = true;
        return false;
    }

    if (qwc == 0u)
    {
        if (!m_ee_dmac.begin_sif0_packet(
                fifo_peek(0u), fifo_peek(1u)))
        {
            m_faulted = true;
            return false;
        }
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            (void)fifo_read();
        }
        return true;
    }

    if ((madr & 15u) != 0u ||
        madr >= 0x02000000u ||
        static_cast<uint64_t>(madr) + 16u > 0x02000000ull ||
        !m_ee_ram.contains(madr, 16u))
    {
        m_faulted = true;
        return false;
    }

    ee::s_ee_u128 quad{};
    quad.m_lo = static_cast<uint64_t>(fifo_peek(0u)) |
                (static_cast<uint64_t>(fifo_peek(1u)) << 32u);
    quad.m_hi = static_cast<uint64_t>(fifo_peek(2u)) |
                (static_cast<uint64_t>(fifo_peek(3u)) << 32u);
    m_ee_ram.write128(madr, &quad);
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        (void)fifo_read();
    }
    m_delivered_bytes += 16u;
    if (!m_ee_dmac.commit_sif0_qwords(1u))
    {
        m_faulted = true;
        return false;
    }
    return true;
}

void c_sif0_dma_transport::pump()
{
    if (m_in_pump || m_faulted)
    {
        return;
    }
    m_in_pump = true;
    constexpr uint32_t k_max_steps = 1u << 18u;
    for (uint32_t step = 0u; step < k_max_steps; ++step)
    {
        bool progressed = false;
        if (consume_one_ee_quadword_or_tag())
        {
            progressed = true;
        }
        if (!m_faulted && produce_one_iop_quadword_or_tag())
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

uint64_t c_sif0_dma_transport::queued_bytes() const
{
    return m_queued_bytes;
}

uint64_t c_sif0_dma_transport::delivered_bytes() const
{
    return m_delivered_bytes;
}

uint32_t c_sif0_dma_transport::fifo_words() const
{
    return m_count;
}

bool c_sif0_dma_transport::faulted() const
{
    return m_faulted;
}
} // namespace iop
