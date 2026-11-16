#include "iop_sif1_dma_transport.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "SIF1 invalid DMA: %s\n", message);
    return 1;
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
} // namespace

int main()
{
    ee::ee_memory ee_ram(4u * 1024u * 1024u);
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&ee_ram);
    dmac.reset();
    iop::c_kernel kernel{};

    iop::c_sif1_dma_transport transport(
        ee_ram, dmac, kernel.cpu(), kernel.hardware());

    constexpr uint32_t k_src = 0x00010000u;
    constexpr uint32_t k_near_end = iop::c_cpu::k_ram_size - 16u;

    // The guest IOP receiver is genuinely armed, but the first IOP
    // DMA tag requests a 32-byte write across its physical RAM limit.
    ee_ram.write32(k_src, 0xc0000000u | k_near_end);
    ee_ram.write32(k_src + 4u, 32u);
    ee_ram.write32(k_src + 8u, 0u);
    ee_ram.write32(k_src + 12u, 0u);
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        ee_ram.write32(k_src + 16u + index * 4u,
                       0xfeed0000u + index);
    }
    if (!kernel.cpu().write32(k_near_end, 0xcdcdcdcdu) ||
        !kernel.cpu().write32(k_near_end + 4u, 0xcdcdcdcdu) ||
        !kernel.cpu().write32(k_near_end + 8u, 0xcdcdcdcdu) ||
        !kernel.cpu().write32(k_near_end + 12u, 0xcdcdcdcdu) ||
        !kernel.hardware().write(
            iop::c_hw::k_sif1_dma_base + 8u, 4u, 0x01000700u) ||
        !write_ee(dmac, 0x10u, k_src) ||
        !write_ee(dmac, 0x20u, 3u) ||
        !write_ee(dmac, 0u, 0x101u))
    {
        return fail("failed to arm malformed destination fixture");
    }

    uint32_t ee_chcr = 0u;
    uint32_t iop_chcr = 0u;
    uint32_t iop_irq = 0u;
    uint32_t word = 0u;
    if (!transport.faulted() ||
        transport.delivered_bytes() != 0u ||
        !read_ee(dmac, 0u, &ee_chcr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) == 0u ||
        (dmac.status() & (1u << 6u)) != 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u, 4u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) == 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_i_stat, 4u, &iop_irq) ||
        (iop_irq & (1u << iop::c_hw::k_dma_irq)) != 0u)
    {
        return fail("malformed tag falsely completed a DMA channel");
    }
    for (uint32_t offset = 0u; offset < 16u; offset += 4u)
    {
        if (!kernel.cpu().read32(k_near_end + offset, &word) ||
            word != 0xcdcdcdcdu)
        {
            return fail("invalid destination modified IOP RAM");
        }
    }

    // A second isolated engine refuses an unmapped source without
    // queuing a byte or consuming a DMA descriptor.
    ee::ee_memory short_ram(4096u);
    ee::c_ee_dmac short_dmac{};
    short_dmac.bind_memory(&short_ram);
    short_dmac.reset();
    iop::c_kernel second_kernel{};
    iop::c_sif1_dma_transport second(
        short_ram, short_dmac,
        second_kernel.cpu(), second_kernel.hardware());
    if (!write_ee(short_dmac, 0x10u, 0x00200000u) ||
        !write_ee(short_dmac, 0x20u, 2u) ||
        !write_ee(short_dmac, 0u, 0x101u) ||
        !second.faulted() ||
        second.queued_bytes() != 0u ||
        second.delivered_bytes() != 0u ||
        !read_ee(short_dmac, 0u, &ee_chcr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) == 0u ||
        short_dmac.status() != 0u ||
        second_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("unmapped source caused transfer or completion");
    }

    std::printf("SIF1 invalid DMA guards: PASS\n");
    return 0;
}
