#include "iop_hw.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP SIF DMA channels: %s\n", message);
    return 1;
}

void put32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t word)
{
    for (uint32_t byte = 0u; byte < 4u; ++byte)
    {
        bytes[offset + byte] =
            static_cast<uint8_t>(word >> (byte * 8u));
    }
}
} // namespace

int main()
{
    iop::c_hw hw{};
    uint32_t value = 0xffffffffu;
    const uint32_t sif0 = iop::c_hw::k_sif0_dma_base;
    const uint32_t sif1 = iop::c_hw::k_sif1_dma_base;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        if (!hw.read(sif0 + 4u * index, 4u, &value) ||
            value != 0u ||
            !hw.read(sif1 + 4u * index, 4u, &value) ||
            value != 0u)
        {
            return fail("DMA channel register reset state");
        }
    }

    if (!hw.write(sif1 + 4u, 4u, 0u) ||
        !hw.write(sif1 + 8u, 4u, 0x01000700u) ||
        !hw.read(sif1 + 8u, 4u, &value) ||
        value != 0x01000700u ||
        hw.pending_interrupts() != 0u ||
        !hw.write(sif0 + 12u, 4u, 0x12340u) ||
        !hw.read(sif0 + 12u, 4u, &value) ||
        value != 0x12340u ||
        hw.read(sif1 + 8u, 2u, &value) ||
        hw.write(sif0 + 8u, 1u, 1u) ||
        hw.read(sif1 + 16u, 4u, &value))
    {
        return fail("DMA register writes, STR persistence or unsupported MMIO");
    }

    if (!hw.complete_dma_channel(10u) ||
        !hw.read(sif1 + 8u, 4u, &value) ||
        value != 0x00000700u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) ||
        (value & (1u << iop::c_hw::k_dma_irq)) == 0u)
    {
        return fail("Explicit DMA completion must clear STR and signal IRQ");
    }
    hw.reset();
    if (!hw.read(sif1 + 8u, 4u, &value) || value != 0u ||
        !hw.read(sif0 + 12u, 4u, &value) || value != 0u ||
        hw.pending_interrupts() != 0u)
    {
        return fail("DMA channel reset failed");
    }

    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t code[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35081534u, // ori t0,t0,0x1534 (SIF1 BCR)
        0xad000000u, // sw zero,0(t0)
        0x25090004u, // addiu t1,t0,4 (CHCR)
        0x3c0a0100u, // lui t2,0x0100
        0x354a0700u, // ori t2,t2,0x0700
        0xad2a0000u, // sw t2,0(t1), arm receive
        0x8d2b0000u, // lw t3,0(t1)
        0x8d0c0000u, // lw t4,0(t0) (BCR)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 10u; ++index)
    {
        put32(rom, index * 4u, code[index]);
    }
    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("Could not start synthetic SIF DMA ROM0");
    }
    const iop::s_kernel_run_result result = kernel.run(20u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_executed != 9u ||
        result.m_guest_pc != 0xbfc00024u ||
        kernel.cpu().state().m_gpr[11u] != 0x01000700u ||
        kernel.cpu().state().m_gpr[12u] != 0u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("Guest SIF1 receiver armed without genuine completion");
    }

    std::printf("IOP SIF DMA channels: PASS\n");
    return 0;
}
