#include "iop_hw.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP DMA global enable: %s\n", message);
    return 1;
}

void put32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t word)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        bytes[offset + index] =
            static_cast<uint8_t>(word >> (index * 8u));
    }
}
} // namespace

int main()
{
    iop::c_hw hw{};
    const uint32_t global = iop::c_hw::k_dma_global_enable;
    uint32_t value = 0xffffffffu;
    if (!hw.read(global, 4u, &value) ||
        value != 0u ||
        !hw.write(global, 4u, 1u) ||
        !hw.read(global, 4u, &value) ||
        value != 1u ||
        hw.pending_interrupts() != 0u ||
        hw.read(global, 1u, &value) ||
        hw.write(global, 2u, 0u) ||
        hw.read(global + 4u, 4u, &value) ||
        hw.write(global + 4u, 4u, 0u))
    {
        return fail("DMA global-enable read/write, MMIO safety or IRQ isolation");
    }
    hw.reset();
    if (!hw.read(global, 4u, &value) ||
        value != 0u || hw.pending_interrupts() != 0u)
    {
        return fail("DMA global enable did not reset");
    }

    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t program[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35081578u, // ori t0,t0,0x1578
        0x8d090000u, // lw t1,0(t0)
        0x35290001u, // ori t1,t1,1
        0xad090000u, // sw t1,0(t0)
        0x8d0a0000u, // lw t2,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        put32(rom, index * 4u, program[index]);
    }
    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("ROM0 guest global-enable fixture failed startup");
    }
    const iop::s_kernel_run_result result = kernel.run(12u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_guest_pc != 0xbfc00018u ||
        result.m_executed != 6u ||
        kernel.cpu().state().m_gpr[9u] != 1u ||
        kernel.cpu().state().m_gpr[10u] != 1u ||
        !kernel.hardware().read(global, 4u, &value) ||
        value != 1u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("R3000 guest global DMA enable or fabricated interrupt");
    }

    std::printf("IOP DMA global enable: PASS\n");
    return 0;
}
