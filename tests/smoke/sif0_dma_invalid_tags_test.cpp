#include "iop_sif0_dma_transport.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "SIF0 invalid descriptor: %s\n", reason);
    return 1;
}

bool ee_write(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t word)
{
    const ee::s_ee_u128 v{word, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_sif0_base + offset, 4u, &v);
}

bool ee_read(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t* result)
{
    ee::s_ee_u128 v{};
    const bool ok = result != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &dmac, ee::c_ee_dmac::k_sif0_base + offset, 4u, &v);
    if (ok)
    {
        *result = static_cast<uint32_t>(v.m_lo);
    }
    return ok;
}

bool test_case(uint32_t source, uint32_t destination,
               uint32_t tag_word)
{
    ee::ee_memory ram(4u * 1024u * 1024u);
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&ram);
    dmac.reset();
    iop::c_kernel kernel{};
    iop::c_sif0_dma_transport fifo(
        ram, dmac, kernel.cpu(), kernel.hardware());

    constexpr uint32_t k_tag_address = 0x00014000u;
    const uint32_t source_qwc = 2u;
    if (!kernel.cpu().write32(
            k_tag_address, source | 0x80000000u) ||
        !kernel.cpu().write32(k_tag_address + 4u,
                               source_qwc * 4u) ||
        !kernel.cpu().write32(k_tag_address + 8u, tag_word) ||
        !kernel.cpu().write32(k_tag_address + 12u, destination) ||
        !kernel.cpu().write32(k_tag_address + 16u, 0u) ||
        !kernel.cpu().write32(k_tag_address + 20u, 0u) ||
        !kernel.hardware().write(
            iop::c_hw::k_sif0_dma_base + 12u,
            4u, k_tag_address) ||
        !ram.contains(0x003fffe0u, 32u))
    {
        return false;
    }
    for (uint32_t address = 0x003fffe0u;
         address < 0x00400000u; address += 4u)
    {
        ram.write32(address, 0xcdcdcdcdu);
    }
    if (!ee_write(dmac, 0x20u, 0u) ||
        !ee_write(dmac, 0u, 0x184u) ||
        !kernel.hardware().write(
            iop::c_hw::k_sif0_dma_base + 8u, 4u,
            0x01000701u))
    {
        return false;
    }
    uint32_t ee_chcr = 0u;
    uint32_t iop_chcr = 0u;
    uint32_t iop_stat = 0u;
    if (!fifo.faulted() || fifo.queued_bytes() != 0u ||
        fifo.delivered_bytes() != 0u || dmac.status() != 0u ||
        !ee_read(dmac, 0u, &ee_chcr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) == 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_sif0_dma_base + 8u, 4u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) == 0u ||
        !kernel.hardware().read(iop::c_hw::k_i_stat,
                                 4u, &iop_stat) ||
        (iop_stat & (1u << iop::c_hw::k_dma_irq)) != 0u)
    {
        return false;
    }
    for (uint32_t address = 0x003fffe0u;
         address < 0x00400000u; address += 4u)
    {
        if (ram.read32(address) != 0xcdcdcdcdu)
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    // Valid IOP source, but an EE destination that crosses physical
    // EE RAM. Reject the complete tag before any destination writes.
    if (!test_case(0x00040000u, 0x003ffff0u, 0x90000002u))
    {
        return fail("cross-boundary EE destination was not rejected");
    }

    // Valid EE destination, but source spills beyond the IOP's 2 MiB.
    if (!test_case(iop::c_cpu::k_ram_size - 16u,
                   0x003ffe00u, 0x90000002u))
    {
        return fail("cross-boundary IOP source was not rejected");
    }

    // TAG_CALL is not a permitted SIF0 EE destination tag here.
    if (!test_case(0x00040000u, 0x003ffe00u, 0xd0000002u))
    {
        return fail("unsupported EE destination tag was accepted");
    }
    std::printf("SIF0 invalid descriptor guards: PASS\n");
    return 0;
}
