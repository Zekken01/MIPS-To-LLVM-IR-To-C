#include "iop_kernel.h"
#include "iop_sif_mmio_link.h"
#include "ee_sif.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "Shared EE/IOP SIF: %s\n", message);
    return 1;
}

void put32(std::vector<uint8_t>& data, uint32_t offset, uint32_t word)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        data[offset + index] =
            static_cast<uint8_t>(word >> (index * 8u));
    }
}
} // namespace

int main()
{
    // A small original R3000 ROM publishes real IOP-owned flags and
    // communication-address data via MMIO, not via an HLE syscall.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t instructions[] =
    {
        0x3c08bd00u, // lui t0,0xbd00
        0x35080010u, // ori t0,t0,0x10 (SMCOM)
        0x3c090004u, // lui t1,0x0004
        0x35294f00u, // ori t1,t1,0x4f00
        0xad090000u, // sw t1,0(t0)
        0x25080020u, // addiu t0,t0,0x20 (SMFLAG)
        0x3c0a0003u, // lui t2,0x3
        0xad0a0000u, // sw t2,0(t0)
        0x8d0b0000u, // lw t3,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 10u; ++index)
    {
        put32(rom, index * 4u, instructions[index]);
    }

    ee::c_ee_sif ee_sif{};
    ee_sif.reset();
    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("Unable to boot guest ROM0 SIF test");
    }
    iop::c_sif_mmio_link::bind(kernel.hardware(), ee_sif);

    uint32_t value = 0xffffffffu;
    if (!ee_sif.get_reg(4u, &value) || value != 0u ||
        kernel.hardware().sif_smflag() != 0u)
    {
        return fail("Linked SIF MMIO fabricated initial readiness");
    }

    const iop::s_kernel_run_result result = kernel.run(20u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_guest_pc != 0xbfc00024u ||
        result.m_executed != 9u ||
        !ee_sif.get_reg(2u, &value) || value != 0x44f00u ||
        !ee_sif.get_reg(4u, &value) || value != 0x30000u ||
        kernel.cpu().state().m_gpr[11u] != 0x30000u ||
        kernel.hardware().sif_smcom() != 0x44f00u ||
        kernel.hardware().sif_smflag() != 0x30000u)
    {
        return fail("R3000 guest store did not reach EE-visible SIF mailbox");
    }

    // The EE may acknowledge only the bits it has actually consumed;
    // guest IOP reads must see the same flags after this EE operation.
    if (!ee_sif.set_reg(4u, 0x10000u) ||
        !kernel.cpu().read32(0xbd000030u, &value) ||
        value != 0x20000u ||
        !ee_sif.set_reg(3u, 0x60000u) ||
        !kernel.cpu().read32(0xbd000020u, &value) ||
        value != 0x60000u ||
        !kernel.cpu().write32(0xbd000020u, 0x40000u) ||
        !ee_sif.get_reg(3u, &value) || value != 0x20000u)
    {
        return fail("EE acknowledgements and IOP MSFLAG writes diverged");
    }
    if (!ee_sif.set_reg(1u, 0x18000u) ||
        !kernel.cpu().read32(0xbd000000u, &value) ||
        value != 0x18000u ||
        kernel.cpu().write32(0xbd000000u, 1u) ||
        kernel.cpu().read32(0xbd000030u, &value) == false ||
        kernel.cpu().write32(0xbd000030u, 0x10000u) == false ||
        !ee_sif.get_reg(4u, &value) || value != 0x30000u)
    {
        return fail("EE-only MSCOM or guest-owned SMFLAG semantics");
    }

    // An IOP reset severs its live EE binding; stale EE flags cannot
    // leak into a new standalone kernel boot.
    kernel.reset();
    if (!kernel.hardware().write(iop::c_hw::k_sif_smflag, 4u, 1u) ||
        kernel.hardware().sif_smflag() != 1u ||
        !ee_sif.get_reg(4u, &value) || value != 0x30000u)
    {
        return fail("Reset preserved stale shared SIF register binding");
    }

    std::printf("Shared EE/IOP SIF: PASS\n");
    return 0;
}
