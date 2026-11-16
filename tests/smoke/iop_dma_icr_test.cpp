#include "iop_hw.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "IOP DMA interrupt-control: %s\n", reason);
    return 1;
}

void put32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        bytes[offset + index] =
            static_cast<uint8_t>(value >> (8u * index));
    }
}
} // namespace

int main()
{
    iop::c_hw hw{};
    uint32_t actual = 0u;
    const uint32_t dma_irq = 1u << iop::c_hw::k_dma_irq;
    if (!hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        actual != 0u ||
        !hw.write(iop::c_hw::k_i_mask, 4u, dma_irq) ||
        !hw.write(iop::c_hw::k_dma_icr, 4u, 0x00800000u) ||
        !hw.write(iop::c_hw::k_dma_icr2, 4u, 0x00040000u) ||
        hw.pending_interrupts() != 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0x00800000u ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        actual != 0x00040000u)
    {
        return fail("DMA ICR default/mask writes fabricated completion");
    }
    if (hw.complete_dma_channel(0u) == false ||
        hw.complete_dma_channel(13u) ||
        hw.pending_interrupts() != 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0x00800000u)
    {
        return fail("Unmasked or invalid DMA event fabricated IRQ");
    }
    if (!hw.write(iop::c_hw::k_dma_icr, 4u, 0x00810000u) ||
        !hw.complete_dma_channel(0u) ||
        hw.pending_interrupts() != dma_irq ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0x81810000u)
    {
        return fail("DMA completion did not latch channel flag/summary/IRQ");
    }
    if (!hw.write(iop::c_hw::k_dma_icr, 4u, 0x01810000u) ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0x00810000u ||
        hw.pending_interrupts() != dma_irq ||
        !hw.write(iop::c_hw::k_i_stat, 4u, ~dma_irq) ||
        hw.pending_interrupts() != 0u)
    {
        return fail("DICR W1C flags must not silently acknowledge I_STAT");
    }
    // SIF0 (channel 9, ICR2 index 2) has an end/tag interrupt
    // independent of the per-channel mask on the IOP.
    if (!hw.complete_dma_channel(9u) ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        (actual & 0x04000000u) == 0u ||
        hw.pending_interrupts() != dma_irq)
    {
        return fail("Explicit SIF0 DMA completion did not deliver IRQ 3");
    }
    hw.reset();
    if (!hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        actual != 0u ||
        hw.pending_interrupts() != 0u)
    {
        return fail("DMA interrupt controller reset retained state");
    }
    if (hw.read(iop::c_hw::k_dma_icr, 1u, &actual) ||
        hw.write(iop::c_hw::k_dma_icr2, 2u, 0u) ||
        hw.read(iop::c_hw::k_dma_icr2 + 4u, 4u, &actual) ||
        hw.write(0x1f8010f0u, 4u, 0u))
    {
        return fail("Unsupported DMA MMIO register/width did not fail");
    }
    if (!hw.write(iop::c_hw::k_dma_icr, 4u, 0x00008000u) ||
        hw.pending_interrupts() != 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        (actual & 0x80000000u) == 0u ||
        !hw.write(iop::c_hw::k_i_mask, 4u, dma_irq) ||
        hw.pending_interrupts() != dma_irq)
    {
        return fail("DICR forced IRQ must still obey INTC I_MASK");
    }

    // Execute actual R3000 reads/writes from the KSEG1 addresses
    // issued by OpenPS2BIOS. This fixture contains no BIOS assets.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t program[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x350810f4u, // ori t0,t0,0x10f4 (DICR)
        0x8d090000u, // lw t1,0(t0)
        0x3c0a0080u, // lui t2,0x0080 (DMA master enable)
        0xad0a0000u, // sw t2,0(t0)
        0x8d0b0000u, // lw t3,0(t0)
        0x3c0cbf80u, // lui t4,0xbf80
        0x358c1574u, // ori t4,t4,0x1574 (DICR2)
        0x3c0d0004u, // lui t5,0x0004 (SIF0 IRQ enable)
        0xad8d0000u, // sw t5,0(t4)
        0x8d8e0000u, // lw t6,0(t4)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 12u; ++index)
    {
        put32(rom, index * 4u, program[index]);
    }

    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("Could not boot synthetic DMA guest ROM");
    }
    const iop::s_kernel_run_result result = kernel.run(24u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_executed != 11u ||
        result.m_guest_pc != 0xbfc0002cu ||
        kernel.cpu().state().m_gpr[9u] != 0u ||
        kernel.cpu().state().m_gpr[11u] != 0x00800000u ||
        kernel.cpu().state().m_gpr[14u] != 0x00040000u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("Guest DMA ICR register sequence or no-spurious-IRQ");
    }

    std::printf("IOP DMA interrupt controller: PASS\n");
    return 0;
}
