#include "iop_sif0_dma_transport.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
constexpr uint32_t k_tag_base = 0x00014000u;
constexpr uint32_t k_source0 = 0x00040000u;
constexpr uint32_t k_source1 = 0x00050000u;
constexpr uint32_t k_destination = 0x00180000u;

int fail(const char* message, uint32_t size, bool sender_first)
{
    std::fprintf(stderr, "SIF0 IOP->EE: %s (size=%u, sender_first=%u)\n",
                 message, size, sender_first ? 1u : 0u);
    return 1;
}

void rom_word(std::vector<uint8_t>& rom, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        rom[at + index] = static_cast<uint8_t>(value >> (index * 8u));
    }
}

bool write_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 word{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_sif0_base + offset, 4u, &word);
}

bool read_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t* value)
{
    ee::s_ee_u128 word{};
    const bool ok = value != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &dmac, ee::c_ee_dmac::k_sif0_base + offset, 4u, &word);
    if (ok)
    {
        *value = static_cast<uint32_t>(word.m_lo);
    }
    return ok;
}

uint32_t pattern(uint32_t size, uint32_t index)
{
    return 0xaad50000u ^ (size * 13u) ^ (index * 0x01020304u);
}

struct s_fixture
{
    ee::ee_memory m_ee_ram{32u * 1024u * 1024u};
    ee::c_ee_dmac m_dmac{};
    iop::c_kernel m_kernel{};
    std::unique_ptr<iop::c_sif0_dma_transport> m_transport;

    s_fixture()
    {
        m_dmac.bind_memory(&m_ee_ram);
        m_dmac.reset();
    }

    bool boot()
    {
        std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
        const uint32_t instructions[] =
        {
            0x3c08bf80u, // lui t0,0xbf80
            0x3508152cu, // ori t0,t0,0x152c (IOP DMA9 TADR)
            0x3c090001u, // lui t1,0x0001
            0x35294000u, // ori t1,t1,0x4000 (tag address)
            0xad090000u, // sw t1,0(t0)
            0x2508fffcu, // addiu t0,t0,-4 (DMA9 CHCR)
            0x3c090100u, // lui t1,0x0100
            0x35290701u, // ori t1,t1,0x0701
            0xad090000u, // sw t1,0(t0), start real DMA9
            0x0000000du  // break
        };
        for (uint32_t index = 0u;
             index < sizeof(instructions) / sizeof(instructions[0]);
             ++index)
        {
            rom_word(rom, index * 4u, instructions[index]);
        }
        if (!m_kernel.start_rom0_boot(rom))
        {
            return false;
        }
        // R3000 reset clears hardware and unbinds callbacks.
        m_transport = std::make_unique<iop::c_sif0_dma_transport>(
            m_ee_ram, m_dmac, m_kernel.cpu(), m_kernel.hardware());
        return true;
    }

    bool run_sender_guest()
    {
        const iop::s_kernel_run_result result = m_kernel.run(32u, 1u);
        return result.m_stop == iop::e_kernel_stop::breakpoint &&
               result.m_executed == 9u &&
               result.m_guest_pc == 0xbfc00024u;
    }
};

int run_case(uint32_t size, bool sender_first)
{
    s_fixture f{};
    if (!f.boot())
    {
        return fail("ROM0 fixture did not start", size, sender_first);
    }
    const uint32_t segments = size >= 32u ? 2u : 1u;
    const uint32_t first_bytes = segments == 2u ? size / 2u : size;
    const uint32_t segment_bytes[2] =
    {
        first_bytes, size - first_bytes
    };
    const uint32_t source[2] = {k_source0, k_source1};
    const uint32_t destination[2] =
    {
        k_destination, k_destination + first_bytes
    };

    for (uint32_t index = 0u; index < segments; ++index)
    {
        const uint32_t qwc = segment_bytes[index] / 16u;
        const uint32_t tag = k_tag_base + index * 16u;
        const uint32_t last = index + 1u == segments;
        const uint32_t tag_flags = last ? 0x80000000u : 0u;
        // OpenPS2BIOS sif.c: IOP DMA9 tag's upper words are
        // EE CNT destination tag; last has IRQ for EE CHCR.TIE.
        if (!f.m_kernel.cpu().write32(tag, source[index] | tag_flags) ||
            !f.m_kernel.cpu().write32(tag + 4u, qwc * 4u) ||
            !f.m_kernel.cpu().write32(
                tag + 8u, 0x10000000u | tag_flags | qwc) ||
            !f.m_kernel.cpu().write32(tag + 12u, destination[index]))
        {
            return fail("failed to create IOP DMA9 guest tag", size, sender_first);
        }

        for (uint32_t offset = 0u;
             offset < segment_bytes[index]; offset += 4u)
        {
            const uint32_t linear = (destination[index] -
                                     k_destination + offset) / 4u;
            if (!f.m_kernel.cpu().write32(
                    source[index] + offset, pattern(size, linear)))
            {
                return fail("failed to populate IOP source RAM", size, sender_first);
            }
        }
    }

    // Two extra words are required by SIF0 header transfer: the final
    // IOP tag reads the following tag header into the FIFO.
    if (!f.m_kernel.cpu().write32(
            k_tag_base + segments * 16u, 0u) ||
        !f.m_kernel.cpu().write32(
            k_tag_base + segments * 16u + 4u, 0u))
    {
        return fail("could not store following-tag header", size, sender_first);
    }

    for (uint32_t at = k_destination - 16u;
         at < k_destination + size + 16u; at += 4u)
    {
        f.m_ee_ram.write32(at, 0xcdcdcdcdu);
    }

    auto arm_ee = [&]()
    {
        return write_ee(f.m_dmac, 0x20u, 0u) &&
               write_ee(f.m_dmac, 0u, 0x184u);
    };

    if (sender_first)
    {
        if (!f.run_sender_guest())
        {
            return fail("guest DMA9 CHCR start did not execute", size, sender_first);
        }
        uint32_t ee_chcr = 0u;
        uint32_t iop_chcr = 0u;
        if (f.m_transport->delivered_bytes() != 0u ||
            f.m_dmac.status() != 0u ||
            !read_ee(f.m_dmac, 0u, &ee_chcr) ||
            (ee_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
            !f.m_kernel.hardware().read(
                iop::c_hw::k_sif0_dma_base + 8u, 4u, &iop_chcr))
        {
            return fail("unarmed EE receiver got a fabricated completion",
                        size, sender_first);
        }
        if (size > 496u && (iop_chcr & 0x01000000u) == 0u)
        {
            return fail("IOP sender completed with full FIFO",
                        size, sender_first);
        }
        if (!arm_ee())
        {
            return fail("failed to arm EE SIF0 receiver", size, sender_first);
        }
    }
    else if (!arm_ee() || !f.run_sender_guest())
    {
        return fail("receiver-first guest DMA9 start failed",
                    size, sender_first);
    }

    uint32_t ee_chcr = 0u;
    uint32_t ee_qwc = 0u;
    uint32_t ee_madr = 0u;
    uint32_t iop_chcr = 0u;
    uint32_t iop_tadr = 0u;
    uint32_t iop_madr = 0u;
    uint32_t iop_istat = 0u;
    uint32_t iop_icr2 = 0u;
    if (f.m_transport->faulted() ||
        f.m_transport->fifo_words() != 0u ||
        f.m_transport->queued_bytes() != size + segments * 16u ||
        f.m_transport->delivered_bytes() != size ||
        !read_ee(f.m_dmac, 0u, &ee_chcr) ||
        !read_ee(f.m_dmac, 0x20u, &ee_qwc) ||
        !read_ee(f.m_dmac, 0x10u, &ee_madr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
        ee_qwc != 0u || ee_madr != k_destination + size ||
        (f.m_dmac.status() & (1u << 5u)) == 0u ||
        !f.m_kernel.hardware().read(
            iop::c_hw::k_sif0_dma_base + 8u, 4u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) != 0u ||
        !f.m_kernel.hardware().read(
            iop::c_hw::k_sif0_dma_base + 12u, 4u, &iop_tadr) ||
        iop_tadr != k_tag_base + segments * 16u ||
        !f.m_kernel.hardware().read(
            iop::c_hw::k_sif0_dma_base, 4u, &iop_madr) ||
        iop_madr != source[segments - 1u] +
                    segment_bytes[segments - 1u] ||
        !f.m_kernel.hardware().read(
            iop::c_hw::k_i_stat, 4u, &iop_istat) ||
        (iop_istat & (1u << iop::c_hw::k_dma_irq)) == 0u ||
        !f.m_kernel.hardware().read(
            iop::c_hw::k_dma_icr2, 4u, &iop_icr2) ||
        (iop_icr2 & (1u << 26u)) == 0u)
    {
        std::fprintf(stderr,
            "status: fault=%u fifo=%u queued=%llu delivered=%llu EEchcr=%08x "
            "EEqwc=%u EEmadr=%08x DSTAT=%08x IOPchcr=%08x TADR=%08x "
            "MADR=%08x ISTAT=%08x ICR2=%08x\n",
            f.m_transport->faulted() ? 1u : 0u, f.m_transport->fifo_words(),
            static_cast<unsigned long long>(f.m_transport->queued_bytes()),
            static_cast<unsigned long long>(f.m_transport->delivered_bytes()),
            ee_chcr, ee_qwc, ee_madr, f.m_dmac.status(), iop_chcr,
            iop_tadr, iop_madr, iop_istat, iop_icr2);
        return fail("DMA5/DMA9 completion state or byte count", size, sender_first);
    }

    uint64_t source_checksum = 0u;
    uint64_t destination_checksum = 0u;
    for (uint32_t at = k_destination - 16u;
         at < k_destination + size + 16u; at += 4u)
    {
        const uint32_t got = f.m_ee_ram.read32(at);
        if (at < k_destination || at >= k_destination + size)
        {
            if (got != 0xcdcdcdcdu)
            {
                return fail("EE destination guard mutated", size, sender_first);
            }
        }
        else
        {
            const uint32_t linear = (at - k_destination) / 4u;
            const uint32_t expected = pattern(size, linear);
            const uint32_t source_address = at < destination[1] || segments == 1u
                ? source[0] + (at - k_destination)
                : source[1] + (at - destination[1]);
            uint32_t source_word = 0u;
            if (!f.m_kernel.cpu().read32(source_address, &source_word) ||
                source_word != expected || got != expected)
            {
                return fail("source/destination mismatch", size, sender_first);
            }
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                source_checksum += static_cast<uint8_t>(
                    source_word >> (lane * 8u));
                destination_checksum += static_cast<uint8_t>(
                    got >> (lane * 8u));
            }
        }
    }
    std::printf(
        "{\"direction\":\"SIF0\",\"ee_channel\":5,\"iop_channel\":9,"
        "\"segments\":%u,\"source\":%u,\"destination\":%u,"
        "\"requested_bytes\":%u,\"transferred_bytes\":%llu,"
        "\"source_checksum\":%llu,\"destination_checksum\":%llu,"
        "\"dstat\":%u,\"iop_dicr2\":%u,\"iop_istat\":%u,"
        "\"sender_first\":%s}\n",
        segments, k_source0, k_destination, size,
        static_cast<unsigned long long>(f.m_transport->delivered_bytes()),
        static_cast<unsigned long long>(source_checksum),
        static_cast<unsigned long long>(destination_checksum),
        f.m_dmac.status(), iop_icr2, iop_istat,
        sender_first ? "true" : "false");
    return 0;
}
} // namespace

int main()
{
    const uint32_t sizes[] = {16u, 32u, 128u, 256u, 4096u};
    for (uint32_t size : sizes)
    {
        if (run_case(size, true) != 0 ||
            run_case(size, false) != 0)
        {
            return 1;
        }
    }
    std::printf("SIF0 IOP-to-EE hardware FIFO: PASS\n");
    return 0;
}
