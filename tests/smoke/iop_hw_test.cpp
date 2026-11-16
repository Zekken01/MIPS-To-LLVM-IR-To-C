#include "iop_hw.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP ROM0 hardware: %s\n", message);
    return 1;
}

void put32(std::vector<uint8_t>& data, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        data[at + index] =
            static_cast<uint8_t>(value >> (8u * index));
    }
}
} // namespace

int main()
{
    iop::c_hw hardware{};
    uint32_t actual = 0u;

    if (!hardware.read(iop::c_hw::k_i_ctrl, 4u, &actual) ||
        actual != 1u ||
        !hardware.read(iop::c_hw::k_i_ctrl, 4u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("I_CTRL read-to-disable reset behavior");
    }
    if (!hardware.write(iop::c_hw::k_i_ctrl, 4u, 0xffffffffu) ||
        !hardware.raise_interrupt(3u) ||
        hardware.raise_interrupt(32u) ||
        !hardware.write(iop::c_hw::k_i_mask, 4u, 1u << 3u) ||
        hardware.pending_interrupts() != (1u << 3u) ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != (1u << 3u) ||
        !hardware.write(iop::c_hw::k_i_stat, 4u, 0xffffffffu) ||
        hardware.pending_interrupts() != (1u << 3u) ||
        !hardware.write(iop::c_hw::k_i_stat, 4u, ~(1u << 3u)) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("IRQ status/mask AND-acknowledge or pending semantics");
    }

    const uint32_t scratch = iop::c_hw::k_scratchpad_base;
    if (!hardware.write(scratch, 4u, 0x11223344u) ||
        !hardware.read(scratch + 1u, 1u, &actual) ||
        actual != 0x33u ||
        !hardware.write(scratch + 2u, 2u, 0xaabbu) ||
        !hardware.read(scratch, 4u, &actual) ||
        actual != 0xaabb3344u ||
        !hardware.write(scratch + 1020u, 4u, 0xcafebabeu) ||
        hardware.write(scratch + 1022u, 4u, 0u) ||
        hardware.read(scratch + 1022u, 4u, &actual) ||
        hardware.read(scratch + 1024u, 1u, &actual) ||
        hardware.read(0x1f801100u, 4u, &actual) ||
        hardware.write(0x1f8010fcu, 4u, 1u))
    {
        return fail("scratchpad size, subword access or unknown MMIO safety");
    }

    // The standalone ROM0 runner has no mounted disc. The CDVD
    // disk-type register must report no media, not an invented game disc.
    if (!hardware.read(iop::c_hw::k_cdvd_disc_type, 1u, &actual) ||
        actual != iop::c_hw::k_cdvd_no_disc ||
        hardware.read(iop::c_hw::k_cdvd_disc_type, 2u, &actual) ||
        hardware.read(iop::c_hw::k_cdvd_disc_type, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_disc_type, 1u, 0x14u) ||
        hardware.read(iop::c_hw::k_cdvd_disc_type - 1u, 1u, &actual))
    {
        return fail("CDVD no-media type access or unsupported width safety");
    }

    // Firmware writes bytes to the emulator's IOP debug console.
    // Other widths, reads, and nearby unmapped addresses must still fault.
    const uint32_t console = iop::c_hw::k_debug_console;
    if (!hardware.write(console, 1u, 'H') ||
        !hardware.write(console, 1u, 'i') ||
        hardware.debug_console() != "Hi" ||
        hardware.debug_console_truncated() ||
        hardware.read(console, 1u, &actual) ||
        hardware.write(console, 2u, 0x1234u) ||
        hardware.write(console, 4u, 0x12345678u) ||
        hardware.write(console + 1u, 1u, 0u))
    {
        return fail("IOP diagnostic console register write/read/width behavior");
    }
    hardware.reset();
    if (!hardware.debug_console().empty() ||
        hardware.debug_console_truncated())
    {
        return fail("IOP diagnostic console reset should clear output");
    }
    for (size_t index = 0u;
         index <= iop::c_hw::k_debug_console_capacity; ++index)
    {
        if (!hardware.write(console, 1u, 'X'))
        {
            return fail("IOP diagnostic console failed bounded byte write");
        }
    }
    if (hardware.debug_console().size() !=
            iop::c_hw::k_debug_console_capacity ||
        !hardware.debug_console_truncated())
    {
        return fail("IOP diagnostic console exceeded bounded capacity");
    }
    hardware.reset();

    if (!hardware.write(iop::c_hw::k_cache_control, 4u, 0x804u) ||
        !hardware.read(iop::c_hw::k_cache_control, 4u, &actual) ||
        actual != 0x804u ||
        hardware.write(iop::c_hw::k_cache_control, 2u, 0xffffu) ||
        hardware.read(iop::c_hw::k_cache_control + 4u, 4u, &actual))
    {
        return fail("cache-control register access validation");
    }

    // The ROM0 entrypoint performs actual R3000 load/store operations
    // against scratchpad, cache control and IOP interrupt mask.
    // These are original synthetic instructions, not copied BIOS data.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t instructions[] =
    {
        0x3c081f80u, // lui t0,0x1f80
        0x24091234u, // addiu t1,zero,0x1234
        0xad090000u, // sw t1,0(t0) (scratchpad)
        0x8d0a0000u, // lw t2,0(t0)
        0x3c0bfffeu, // lui t3,0xfffe
        0x356b0130u, // ori t3,t3,0x0130
        0x240c0804u, // addiu t4,zero,0x804
        0xad6c0000u, // sw t4,0(t3) (cache control)
        0x8d6d0000u, // lw t5,0(t3)
        0x3c0e1f80u, // lui t6,0x1f80
        0x35ce1074u, // ori t6,t6,0x1074
        0x240f0008u, // addiu t7,zero,8
        0xadcf0000u, // sw t7,0(t6) (I_MASK)
        0x8dd80000u, // lw t8,0(t6)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 15u; ++index)
    {
        put32(rom, index * 4u, instructions[index]);
    }

    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 kernel startup rejected valid image");
    }

    const iop::s_kernel_run_result executed = kernel.run(32u, 4u);
    if (executed.m_stop != iop::e_kernel_stop::breakpoint ||
        executed.m_guest_pc != 0xbfc00038u ||
        executed.m_executed != 14u ||
        kernel.cpu().state().m_gpr[10u] != 0x1234u ||
        kernel.cpu().state().m_gpr[13u] != 0x804u ||
        kernel.cpu().state().m_gpr[24u] != 8u ||
        kernel.hardware().cache_control() != 0x804u ||
        !kernel.hardware().read(iop::c_hw::k_i_mask, 4u, &actual) ||
        actual != 8u ||
        !kernel.cpu().read32(0x1f800000u, &actual) ||
        actual != 0x1234u ||
        !kernel.cpu().read32(0xbf800000u, &actual) ||
        actual != 0x1234u)
    {
        return fail("guest instruction stream did not access mapped hardware");
    }

    // Actual guest SB from KSEG1 into the diagnostic console, without
    // allowing reads or changing memory state to fake hardware progress.
    const uint32_t debug_instructions[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35082080u, // ori t0,t0,0x2080
        0x24090041u, // addiu t1,zero,'A'
        0xa1090000u, // sb t1,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        put32(rom, index * 4u, debug_instructions[index]);
    }
    iop::c_kernel debug_kernel{};
    if (!debug_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 console test failed to install ROM");
    }
    const iop::s_kernel_run_result logged = debug_kernel.run(16u, 4u);
    if (logged.m_stop != iop::e_kernel_stop::breakpoint ||
        logged.m_executed != 4u ||
        debug_kernel.hardware().debug_console() != "A")
    {
        return fail("Guest SB to IOP diagnostic console was not captured");
    }

    // Guest LBU from CDVD using the KSEG1 address seen in OpenPS2BIOS.
    const uint32_t cdvd_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x3508200fu, // ori t0,t0,0x200f
        0x91090000u, // lbu t1,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        put32(rom, index * 4u, cdvd_instructions[index]);
    }
    iop::c_kernel cdvd_kernel{};
    if (!cdvd_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 CDVD test failed to install ROM");
    }
    const iop::s_kernel_run_result cdvd = cdvd_kernel.run(16u, 4u);
    if (cdvd.m_stop != iop::e_kernel_stop::breakpoint ||
        cdvd.m_executed != 3u ||
        cdvd_kernel.cpu().state().m_gpr[9u] !=
            iop::c_hw::k_cdvd_no_disc)
    {
        return fail("Guest LBU did not read CDVD no-media state");
    }

    // OpenPS2BIOS resets CDVD DMA3 via an actual SW through KSEG1.
    const uint32_t dma3_guest[] =
    {
        0x3c01bf80u, // lui at,0xbf80
        0x342110b8u, // ori at,at,0x10b8: CDVD DMA3 CHCR
        0xac200000u, // sw zero,0(at)
        0x8c280000u, // lw t0,0(at)
        0x0000000du  // guest BREAK
    };
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        put32(rom, index * 4u, dma3_guest[index]);
    }
    iop::c_kernel primary_dma_kernel{};
    if (!primary_dma_kernel.start_rom0_boot(rom))
    {
        return fail("IOP primary DMA ROM0 fixture did not start");
    }
    const iop::s_kernel_run_result dma3_run =
        primary_dma_kernel.run(16u, 4u);
    if (dma3_run.m_stop != iop::e_kernel_stop::breakpoint ||
        dma3_run.m_executed != 4u ||
        primary_dma_kernel.cpu().state().m_gpr[8u] != 0u)
    {
        return fail("Guest DMA3 reset SW/LW failed or faulted");
    }
    iop::c_hw& primary_dma_hw = primary_dma_kernel.hardware();
    const uint32_t dma3 = iop::c_hw::k_primary_dma_base + 0x30u;
    if (!primary_dma_hw.write(dma3, 4u, 0x00123450u) ||
        !primary_dma_hw.write(dma3 + 4u, 4u, 0x00010020u) ||
        !primary_dma_hw.write(dma3 + 8u, 4u, 0x01000000u) ||
        !primary_dma_hw.read(dma3 + 8u, 4u, &actual) ||
        actual != 0x01000000u ||
        primary_dma_hw.pending_interrupts() != 0u)
    {
        return fail("Primary DMA3 register write fabricated completion");
    }
    if (!primary_dma_hw.write(dma3 + 8u, 4u, 0u) ||
        !primary_dma_hw.read(dma3 + 8u, 4u, &actual) ||
        actual != 0u ||
        primary_dma_hw.write(dma3 + 12u, 4u, 0u) ||
        primary_dma_hw.read(dma3 + 12u, 4u, &actual) ||
        primary_dma_hw.write(dma3 + 8u, 1u, 0u))
    {
        return fail("Primary DMA CHCR stop or MMIO bounds failed");
    }
    primary_dma_kernel.reset();
    if (!primary_dma_hw.read(dma3, 4u, &actual) || actual != 0u ||
        !primary_dma_hw.read(dma3 + 4u, 4u, &actual) || actual != 0u)
    {
        return fail("IOP primary DMA registers survived reset");
    }

    // The cache-control register is address-specific. The IOP bus
    // must not turn all of KSEG2/KSEG3 into a permissive RAM alias.
    if (kernel.cpu().write32(0xfffe0134u, 0u) ||
        kernel.cpu().read32(0xfffe0134u, &actual) ||
        kernel.cpu().read32(0x1f808000u, &actual))
    {
        return fail("unimplemented high control/MMIO address did not fault");
    }

    if (!kernel.hardware().raise_interrupt(3u) ||
        kernel.hardware().pending_interrupts() != 8u)
    {
        return fail("ROM I_MASK did not gate host-raised IRQ state");
    }
    kernel.reset();
    if (kernel.hardware().pending_interrupts() != 0u ||
        !kernel.hardware().read(iop::c_hw::k_i_ctrl, 4u, &actual) ||
        actual != 1u ||
        !kernel.hardware().read(iop::c_hw::k_i_mask, 4u, &actual) ||
        actual != 0u ||
        kernel.cpu().write32(0x1f800000u, 123u))
    {
        return fail("IOP reset retained stale MMIO or stale bus attachment");
    }

    std::printf("IOP ROM0 hardware: PASS\n");
    return 0;
}
