#include "iop_hw.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP SIF handoff: %s\n", message);
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
    iop::c_hw hardware{};
    uint32_t value = 0u;
    if (!hardware.read(iop::c_hw::k_sif_smcom, 4u, &value) ||
        value != 0u ||
        !hardware.read(iop::c_hw::k_sif_smflag, 4u, &value) ||
        value != 0u ||
        hardware.write(iop::c_hw::k_sif_mscom, 4u, 1u) ||
        !hardware.write(iop::c_hw::k_sif_smcom, 4u, 0x12340u) ||
        hardware.sif_smcom() != 0x12340u ||
        !hardware.write(iop::c_hw::k_sif_smflag, 4u, 0x10000u) ||
        !hardware.write(iop::c_hw::k_sif_smflag, 4u, 0x20000u) ||
        hardware.sif_smflag() != 0x30000u ||
        hardware.pending_interrupts() != 0u ||
        hardware.read(iop::c_hw::k_sif_smflag, 1u, &value) ||
        hardware.write(iop::c_hw::k_sif_smflag, 2u, 0u) ||
        hardware.write(0x1d000034u, 4u, 1u))
    {
        return fail("SIF IOP handoff/flag accumulation or MMIO safety");
    }
    // Without EE acknowledgement these firmware-published flags must
    // remain set. Only the EE side is allowed to clear SMFLAG.
    hardware.ee_clear_smflag(0x10000u);
    if (hardware.sif_smflag() != 0x20000u)
    {
        return fail("EE acknowledgement failed to clear selected bits");
    }
    hardware.ee_write_mscom(0xabcdu);
    hardware.ee_set_msflag(0x70000u);
    if (!hardware.read(iop::c_hw::k_sif_mscom, 4u, &value) ||
        value != 0xabcdu ||
        !hardware.read(iop::c_hw::k_sif_msflag, 4u, &value) ||
        value != 0x70000u ||
        !hardware.write(iop::c_hw::k_sif_msflag, 4u, 0x10000u) ||
        !hardware.read(iop::c_hw::k_sif_msflag, 4u, &value) ||
        value != 0x60000u)
    {
        return fail("EE->IOP communication/interrupt flag acknowledgement");
    }
    hardware.reset();
    if (!hardware.read(iop::c_hw::k_sif_smcom, 4u, &value) ||
        value != 0u ||
        hardware.sif_smflag() != 0u ||
        !hardware.read(iop::c_hw::k_sif_msflag, 4u, &value) ||
        value != 0u)
    {
        return fail("SIF registers must reset without host ready flags");
    }

    // This guest script writes real IOP KSEG1 SIF addresses.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t program[] =
    {
        0x3c08bd00u, // lui t0,0xbd00
        0x35080010u, // ori t0,t0,0x0010 (IOP subaddress)
        0x3c090001u, // lui t1,0x0001
        0x35292340u, // ori t1,t1,0x2340
        0xad090000u, // sw t1,0(t0)
        0x25080020u, // addiu t0,t0,0x20 (SMFLAG)
        0x3c0a0003u, // lui t2,0x0003
        0xad0a0000u, // sw t2,0(t0), firmware announces flags
        0x8d0b0000u, // lw t3,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 10u; ++index)
    {
        put32(rom, index * 4u, program[index]);
    }
    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("Guest SIF register ROM0 test could not start");
    }
    const iop::s_kernel_run_result result = kernel.run(20u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_guest_pc != 0xbfc00024u ||
        result.m_executed != 9u ||
        kernel.hardware().sif_smcom() != 0x12340u ||
        kernel.hardware().sif_smflag() != 0x30000u ||
        kernel.cpu().state().m_gpr[11u] != 0x30000u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("Guest SIF flags were not published by actual R3000 stores");
    }

    std::printf("IOP SIF handoff: PASS\n");
    return 0;
}
