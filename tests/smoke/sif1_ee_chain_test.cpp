#include "iop_sif1_dma_transport.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
constexpr uint32_t k_chain = 0x00010000u;
constexpr uint32_t k_ref_data = 0x00012000u;
constexpr uint32_t k_destination = 0x00018000u;

int fail(const char* message)
{
    std::fprintf(stderr, "SIF1 EE source chain: %s\n", message);
    return 1;
}

void rom_word(std::vector<uint8_t>& rom, uint32_t at, uint32_t word)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        rom[at + index] = static_cast<uint8_t>(word >> (index * 8u));
    }
}

bool arm_iop_guest(iop::c_kernel& kernel)
{
    const iop::s_kernel_run_result run = kernel.run(32u, 1u);
    return run.m_stop == iop::e_kernel_stop::breakpoint &&
           run.m_executed == 5u &&
           run.m_guest_pc == 0xbfc00014u;
}

bool write_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 word{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_sif1_base + offset, 4u, &word);
}

bool read_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t* value)
{
    ee::s_ee_u128 word{};
    const bool ok = value != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &dmac, ee::c_ee_dmac::k_sif1_base + offset, 4u, &word);
    if (ok)
    {
        *value = static_cast<uint32_t>(word.m_lo);
    }
    return ok;
}

uint32_t pattern(uint32_t segment, uint32_t word)
{
    return 0xabc00000u ^ (segment * 0x01010101u) ^ word;
}

struct s_fixture
{
    ee::ee_memory m_ram{4u * 1024u * 1024u};
    ee::c_ee_dmac m_dmac{};
    iop::c_kernel m_kernel{};
    iop::c_sif1_dma_transport m_transport;

    s_fixture()
        : m_transport(m_ram, m_dmac, m_kernel.cpu(),
                      m_kernel.hardware())
    {
        m_dmac.bind_memory(&m_ram);
        m_dmac.reset();
    }

    bool boot()
    {
        // Real R3000 SW instruction starts IOP DMA10.
        std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
        const uint32_t instructions[] =
        {
            0x3c08bf80u, // lui t0,0xbf80
            0x35081538u, // ori t0,t0,0x1538
            0x3c090100u, // lui t1,0x0100
            0x35290700u, // ori t1,t1,0x0700
            0xad090000u, // sw t1,0(t0)
            0x0000000du  // break
        };
        for (uint32_t index = 0u;
             index < sizeof(instructions) / sizeof(instructions[0]);
             ++index)
        {
            rom_word(rom, index * 4u, instructions[index]);
        }
        return m_kernel.start_rom0_boot(rom);
    }

    bool start_ee(uint32_t chcr = 0x105u)
    {
        return write_ee(m_dmac, 0x20u, 0u) &&
               write_ee(m_dmac, 0x30u, k_chain) &&
               write_ee(m_dmac, 0u, chcr);
    }

    void make_iop_header(uint32_t at, uint32_t count)
    {
        m_ram.write32(at, 0x40000000u | k_destination);
        m_ram.write32(at + 4u, count);
        m_ram.write32(at + 8u, 0u);
        m_ram.write32(at + 12u, 0u);
    }
};

bool run_multitag_chain()
{
    s_fixture f{};
    if (!f.boot())
    {
        return false;
    }

    // CNT: IOP 16-byte destination header as inline EE payload.
    f.m_ram.write32(k_chain, 0x10000001u);
    f.m_ram.write32(k_chain + 4u, 0u);
    f.make_iop_header(k_chain + 0x10u, 48u);

    // NEXT: one inline payload and nonsequential next tag.
    f.m_ram.write32(k_chain + 0x20u, 0x20000001u);
    f.m_ram.write32(k_chain + 0x24u, k_chain + 0x80u);
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        f.m_ram.write32(k_chain + 0x30u + i * 4u, pattern(0u, i));
    }

    // REF: payload stored outside the chain, followed by END.
    f.m_ram.write32(k_chain + 0x80u, 0x30000001u);
    f.m_ram.write32(k_chain + 0x84u, k_ref_data);
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        f.m_ram.write32(k_ref_data + i * 4u, pattern(1u, i));
    }

    f.m_ram.write32(k_chain + 0x90u, 0x70000001u);
    f.m_ram.write32(k_chain + 0x94u, 0u);
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        f.m_ram.write32(k_chain + 0xa0u + i * 4u, pattern(2u, i));
    }

    for (uint32_t at = k_destination - 16u;
         at < k_destination + 64u; at += 4u)
    {
        if (!f.m_kernel.cpu().write32(at, 0xcdcdcdcdu))
        {
            return false;
        }
    }
    // Sender-first: a complete EE chain may fill its FIFO without an
    // IOP receiver, but no IOP completion or RAM writes may occur.
    if (!f.start_ee() || f.m_transport.faulted() ||
        f.m_transport.queued_bytes() != 64u ||
        f.m_transport.delivered_bytes() != 0u)
    {
        return false;
    }

    uint32_t chcr = 0u;
    uint32_t tadr = 0u;
    if (!read_ee(f.m_dmac, 0u, &chcr) ||
        !read_ee(f.m_dmac, 0x30u, &tadr) ||
        (chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
        tadr != k_chain + 0x90u ||
        (f.m_dmac.status() & (1u << 6u)) == 0u ||
        f.m_kernel.hardware().pending_interrupts() != 0u ||
        !arm_iop_guest(f.m_kernel))
    {
        return false;
    }

    if (f.m_transport.faulted() ||
        f.m_transport.fifo_words() != 0u ||
        f.m_transport.delivered_bytes() != 48u)
    {
        return false;
    }

    uint32_t iop_chcr = 0u;
    uint32_t iop_stat = 0u;
    if (!f.m_kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u, 4u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) != 0u ||
        !f.m_kernel.hardware().read(iop::c_hw::k_i_stat, 4u, &iop_stat) ||
        (iop_stat & (1u << iop::c_hw::k_dma_irq)) == 0u)
    {
        return false;
    }

    for (uint32_t at = k_destination - 16u;
         at < k_destination + 64u; at += 4u)
    {
        uint32_t actual = 0u;
        if (!f.m_kernel.cpu().read32(at, &actual))
        {
            return false;
        }
        if (at < k_destination || at >= k_destination + 48u)
        {
            if (actual != 0xcdcdcdcdu)
            {
                return false;
            }
        }
        else
        {
            const uint32_t index = (at - k_destination) / 4u;
            const uint32_t expected = pattern(index / 4u, index % 4u);
            const uint32_t source = index < 4u
                ? k_chain + 0x30u + index * 4u
                : index < 8u
                ? k_ref_data + (index - 4u) * 4u
                : k_chain + 0xa0u + (index - 8u) * 4u;
            if (actual != expected ||
                f.m_ram.read32(source) != expected)
            {
                return false;
            }
        }
    }

    std::printf(
        "{\"direction\":\"SIF1\",\"mode\":\"EE_CHAIN\","
        "\"ee_channel\":6,\"iop_channel\":10,"
        "\"segments\":4,\"source\":%u,\"destination\":%u,"
        "\"requested_bytes\":48,\"transferred_bytes\":%llu,"
        "\"final_tadr\":%u,\"dstat\":%u,\"iop_istat\":%u}\n",
        k_chain, k_destination,
        static_cast<unsigned long long>(f.m_transport.delivered_bytes()),
        tadr, f.m_dmac.status(), iop_stat);
    return true;
}

bool run_tie_irq()
{
    s_fixture f{};
    if (!f.boot())
    {
        return false;
    }
    f.m_ram.write32(k_chain, 0x10000001u);
    f.m_ram.write32(k_chain + 4u, 0u);
    f.make_iop_header(k_chain + 0x10u, 16u);

    // REF has IRQ set. TIE must terminate before subsequent END.
    f.m_ram.write32(k_chain + 0x20u, 0xb0000001u);
    f.m_ram.write32(k_chain + 0x24u, k_ref_data);
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        f.m_ram.write32(k_ref_data + i * 4u, pattern(3u, i));
    }
    f.m_ram.write32(k_chain + 0x30u, 0x70000001u);
    f.m_ram.write32(k_chain + 0x34u, 0x00fff000u); // unreachable

    if (!arm_iop_guest(f.m_kernel) ||
        !f.start_ee(0x185u) ||
        f.m_transport.faulted() ||
        f.m_transport.delivered_bytes() != 16u)
    {
        return false;
    }
    uint32_t tadr = 0u;
    uint32_t chcr = 0u;
    if (!read_ee(f.m_dmac, 0x30u, &tadr) ||
        !read_ee(f.m_dmac, 0u, &chcr) ||
        tadr != k_chain + 0x30u ||
        (chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (f.m_dmac.status() & (1u << 6u)) == 0u)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        uint32_t received = 0u;
        if (!f.m_kernel.cpu().read32(
                k_destination + i * 4u, &received) ||
            received != pattern(3u, i))
        {
            return false;
        }
    }
    return true;
}

bool run_bad_tag()
{
    s_fixture f{};
    if (!f.boot())
    {
        return false;
    }
    // CALL is not yet supported: reject it, not a disguised END.
    f.m_ram.write32(k_chain, 0x50000001u);
    f.m_ram.write32(k_chain + 4u, k_ref_data);
    uint32_t before = 0u;
    if (!f.m_kernel.cpu().write32(k_destination, 0xcdcdcdcdu) ||
        !f.start_ee() ||
        !read_ee(f.m_dmac, 0u, &before) ||
        (before & ee::c_ee_dmac::k_chcr_str) == 0u ||
        !f.m_transport.faulted() ||
        f.m_transport.queued_bytes() != 0u ||
        f.m_transport.delivered_bytes() != 0u ||
        f.m_dmac.status() != 0u)
    {
        return false;
    }
    uint32_t actual = 0u;
    return f.m_kernel.cpu().read32(k_destination, &actual) &&
           actual == 0xcdcdcdcdu;
}
} // namespace

int main()
{
    if (!run_multitag_chain())
    {
        return fail("CNT/NEXT/REF/END guest payload or TADR state");
    }
    if (!run_tie_irq())
    {
        return fail("TIE+IRQ did not terminate the source chain");
    }
    if (!run_bad_tag())
    {
        return fail("unsupported CALL tag corrupted memory or completed");
    }
    std::printf("SIF1 EE source chain: PASS\n");
    return 0;
}
