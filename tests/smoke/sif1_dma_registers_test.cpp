#include "ee_dmac.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "SIF1 DMA registers: %s\n", message);
    return 1;
}

bool write_ee(ee::c_ee_dmac& dmac, uint32_t address, uint32_t value)
{
    const ee::s_ee_u128 word{value, 0u};
    return ee::c_ee_dmac::mmio_write(&dmac, address, 4u, &word);
}

bool read_ee(ee::c_ee_dmac& dmac, uint32_t address, uint32_t expected)
{
    ee::s_ee_u128 word{};
    return ee::c_ee_dmac::mmio_read(&dmac, address, 4u, &word) &&
           word.m_lo == expected && word.m_hi == 0u;
}

void put32(std::vector<uint8_t>& rom, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        rom[at + index] = static_cast<uint8_t>(value >> (index * 8u));
    }
}
} // namespace

int main()
{
    ee::c_ee_dmac dmac{};
    dmac.reset();
    constexpr uint32_t k_ee_sif1 = ee::c_ee_dmac::k_sif1_base;
    constexpr uint32_t k_ee_sif0 = ee::c_ee_dmac::k_sif0_base;

    if (!read_ee(dmac, k_ee_sif1, 0u) ||
        !read_ee(dmac, k_ee_sif1 + 0x10u, 0u) ||
        !read_ee(dmac, k_ee_sif1 + 0x20u, 0u) ||
        !read_ee(dmac, k_ee_sif1 + 0x30u, 0u) ||
        !read_ee(dmac, k_ee_sif0, 0u))
    {
        return fail("EE SIF1 channel did not reset to idle");
    }

    // These guest-visible writes only arm channel 6. With no linked
    // transport they cannot fabricate payload, completion, or IRQ.
    if (!write_ee(dmac, k_ee_sif1 + 0x10u, 0x00100000u) ||
        !write_ee(dmac, k_ee_sif1 + 0x20u, 0x10003u) ||
        !write_ee(dmac, k_ee_sif1 + 0x30u, 0x00101000u) ||
        !write_ee(dmac, k_ee_sif1, 0x101u) ||
        !read_ee(dmac, k_ee_sif1 + 0x10u, 0x00100000u) ||
        !read_ee(dmac, k_ee_sif1 + 0x20u, 3u) ||
        !read_ee(dmac, k_ee_sif1 + 0x30u, 0x00101000u) ||
        !read_ee(dmac, k_ee_sif1, 0x101u) ||
        !read_ee(dmac, k_ee_sif0, 0u) ||
        dmac.status() != 0u ||
        dmac.pending() != 0u)
    {
        return fail("EE SIF1 registers or missing-receiver STR handling");
    }

    ee::s_ee_u128 word{};
    if (ee::c_ee_dmac::mmio_read(&dmac, k_ee_sif1, 2u, &word) ||
        ee::c_ee_dmac::mmio_write(&dmac, k_ee_sif1 + 0x80u, 4u, &word) ||
        ee::c_ee_dmac::mmio_read(&dmac, k_ee_sif1 + 0x04u, 4u, &word))
    {
        return fail("unsupported SIF1 MMIO width or register accepted");
    }

    // Execute real R3000 stores/loads into IOP DMA10. The channel is
    // armed but no EE sender is connected, so no interrupt may occur.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t instructions[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35081530u, // ori t0,t0,0x1530 (DMA10 MADR)
        0x3c090002u, // lui t1,2
        0xad090000u, // sw t1,0(t0)
        0x25080004u, // addiu t0,t0,4 (BCR)
        0x34090008u, // ori t1,zero,8
        0xad090000u, // sw t1,0(t0)
        0x25080004u, // addiu t0,t0,4 (CHCR)
        0x3c090100u, // lui t1,0x0100
        0x35290700u, // ori t1,t1,0x0700
        0xad090000u, // sw t1,0(t0)
        0x8d0a0000u, // lw t2,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u;
         index < sizeof(instructions) / sizeof(instructions[0]); ++index)
    {
        put32(rom, index * 4u, instructions[index]);
    }
    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("could not start R3000 DMA10 fixture");
    }
    const iop::s_kernel_run_result result = kernel.run(32u, 1u);
    uint32_t value = 0u;
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_executed != 12u ||
        result.m_guest_pc != 0xbfc00030u ||
        kernel.cpu().state().m_gpr[10u] != 0x01000700u ||
        !kernel.hardware().read(iop::c_hw::k_sif1_dma_base, 4u, &value) ||
        value != 0x00020000u ||
        !kernel.hardware().read(iop::c_hw::k_sif1_dma_base + 4u, 4u, &value) ||
        value != 8u ||
        !kernel.hardware().read(iop::c_hw::k_sif1_dma_base + 8u, 4u, &value) ||
        value != 0x01000700u ||
        !kernel.hardware().read(iop::c_hw::k_i_stat, 4u, &value) ||
        (value & (1u << iop::c_hw::k_dma_irq)) != 0u)
    {
        return fail("R3000 DMA10 programming fabricated completion or lost state");
    }

    dmac.reset();
    if (!read_ee(dmac, k_ee_sif1, 0u) ||
        !read_ee(dmac, k_ee_sif1 + 0x10u, 0u) ||
        !read_ee(dmac, k_ee_sif1 + 0x20u, 0u) ||
        !read_ee(dmac, k_ee_sif1 + 0x30u, 0u) ||
        dmac.status() != 0u)
    {
        return fail("EE SIF1 reset preserved active DMA state");
    }

    std::printf("SIF1 DMA register foundation: PASS\n");
    return 0;
}
