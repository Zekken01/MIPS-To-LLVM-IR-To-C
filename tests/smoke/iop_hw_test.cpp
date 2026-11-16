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

    // Official OpenPS2BIOS stores zero to $at=0xBF8010B8 at
    // PC 0x0001CA00: primary DMA3/CD-ROM CHCR through KSEG1.
    // Register programming is not an implicit DMA completion.
    const uint32_t dma3 = iop::c_hw::k_cdrom_dma_base;
    if (!hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u ||
        !hardware.write(dma3, 4u, 0x00001000u) ||
        !hardware.write(dma3 + 4u, 4u, 0x00000020u) ||
        !hardware.write(dma3 + 8u, 4u, 0u) ||
        !hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u ||
        !hardware.write(dma3 + 8u, 4u, 0x41000200u) ||
        !hardware.read(dma3 + 8u, 4u, &actual) ||
        actual != 0x41000200u ||
        hardware.pending_interrupts() != 0u ||
        !hardware.write(dma3 + 8u, 4u, 0u) ||
        !hardware.read(dma3, 4u, &actual) || actual != 0x1000u ||
        !hardware.read(dma3 + 4u, 4u, &actual) || actual != 0x20u ||
        !hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u ||
        hardware.read(dma3 + 12u, 4u, &actual) ||
        hardware.write(dma3 + 8u, 2u, 0u))
    {
        return fail("primary IOP DMA3 CHCR pending start/reset semantics");
    }
    hardware.reset();
    if (!hardware.read(dma3, 4u, &actual) || actual != 0u ||
        !hardware.read(dma3 + 4u, 4u, &actual) || actual != 0u ||
        !hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u)
    {
        return fail("primary DMA3 registers survived reset");
    }

    // Readiness is a controller state, NOT proof of an inserted disc.
    // The idle no-disc state reports N_READY=0x40 and disc type=0.
    if (!hardware.read(iop::c_hw::k_cdvd_ncmd_status, 1u, &actual) ||
        actual != iop::c_hw::k_cdvd_ncmd_ready ||
        hardware.read(iop::c_hw::k_cdvd_ncmd_status, 2u, &actual) ||
        hardware.read(iop::c_hw::k_cdvd_ncmd_status, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u, 0u))
    {
        return fail("CDVD N-command idle readiness or unsupported command write");
    }
    // CDVD I_STAT (0x1F402008) is a distinct byte-wide W1C
    // interrupt status register; acknowledging zero pending reasons
    // must not synthesize an interrupt or complete a command.
    if (!hardware.read(iop::c_hw::k_cdvd_i_stat, 1u, &actual) ||
        actual != 0u ||
        !hardware.write(iop::c_hw::k_cdvd_i_stat, 1u, 0xffu) ||
        !hardware.read(iop::c_hw::k_cdvd_i_stat, 1u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u ||
        hardware.read(iop::c_hw::k_cdvd_i_stat, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_i_stat, 2u, 1u))
    {
        return fail("CDVD interrupt reason W1C or unsupported width");
    }

    // OpenPS2BIOS clears CDVD DEC_SET at 0xBF40203A. Preserve the
    // 8-bit control value without treating the store as an I/O command.
    if (!hardware.read(iop::c_hw::k_cdvd_dec_set, 1u, &actual) ||
        actual != 0u ||
        !hardware.write(iop::c_hw::k_cdvd_dec_set, 1u, 0x15au) ||
        !hardware.read(iop::c_hw::k_cdvd_dec_set, 1u, &actual) ||
        actual != 0x5au ||
        hardware.read(iop::c_hw::k_cdvd_dec_set, 2u, &actual) ||
        hardware.read(iop::c_hw::k_cdvd_dec_set, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_dec_set, 2u, 0u) ||
        hardware.write(iop::c_hw::k_cdvd_dec_set + 1u, 1u, 0u) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("CDVD DEC_SET byte register storage/width semantics");
    }
    hardware.reset();
    if (!hardware.read(iop::c_hw::k_cdvd_dec_set, 1u, &actual) ||
        actual != 0u)
    {
        return fail("CDVD DEC_SET retained state after reset");
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

    // Execute original ROM0-style instructions against the KSEG1 DMA3
    // CHCR alias. This test never starts or completes a transfer.
    const uint32_t dma3_instructions[] =
    {
        0x3c01bf80u, // lui at,0xbf80
        0x342110b8u, // ori at,at,0x10b8
        0xac200000u, // sw zero,0(at), observed ROM0 opcode
        0x8c290000u, // lw t1,0(at)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        put32(rom, index * 4u, dma3_instructions[index]);
    }
    iop::c_kernel dma3_kernel{};
    if (!dma3_kernel.start_rom0_boot(rom))
    {
        return fail("DMA3 KSEG1 fixture failed to install guest ROM");
    }
    const iop::s_kernel_run_result dma3_result =
        dma3_kernel.run(16u, 4u);
    if (dma3_result.m_stop != iop::e_kernel_stop::breakpoint ||
        dma3_result.m_executed != 4u ||
        dma3_kernel.cpu().state().m_gpr[9u] != 0u ||
        dma3_kernel.cpu().last_memory_fault().m_valid ||
        dma3_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest DMA3 CHCR reset faulted or created phantom IRQ");
    }

    // Actual ROM0-style CDVD DMA3 CHCR start (0x41000200). This is
    // a *pending* request: STR remains set until a real transport
    // completes the work. Neither data nor an IRQ may be fabricated.
    const uint32_t dma3_start_instructions[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x350810b8u, // ori t0,t0,0x10b8
        0x3c094100u, // lui t1,0x4100
        0x35290200u, // ori t1,t1,0x0200
        0xad090000u, // sw t1,0(t0)
        0x8d0a0000u, // lw t2,0(t0)
        0xad000000u, // sw zero,0(t0), guest stop
        0x8d0b0000u, // lw t3,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        put32(rom, index * 4u, dma3_start_instructions[index]);
    }
    iop::c_kernel dma3_start_kernel{};
    if (!dma3_start_kernel.start_rom0_boot(rom))
    {
        return fail("DMA3 start KSEG1 fixture failed to install guest ROM");
    }
    const iop::s_kernel_run_result dma3_start =
        dma3_start_kernel.run(16u, 4u);
    if (dma3_start.m_stop != iop::e_kernel_stop::breakpoint ||
        dma3_start.m_executed != 8u ||
        dma3_start_kernel.cpu().state().m_gpr[10u] != 0x41000200u ||
        dma3_start_kernel.cpu().state().m_gpr[11u] != 0u ||
        dma3_start_kernel.cpu().last_memory_fault().m_valid ||
        dma3_start_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest DMA3 CHCR start/stop fabricated DMA or IRQ");
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

    // Guest LBU from CDVD N-command readiness and disc-type via KSEG1.
    // The first LBU matches the real faulting ROM0 access at 0x0001CA14.
    const uint32_t cdvd_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x35082005u, // ori t0,t0,0x2005
        0x91090000u, // lbu t1,0(t0): ready=0x40
        0x3508200fu, // ori t0,t0,0x200f
        0x910a0000u, // lbu t2,0(t0): disc type=0x00
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 6u; ++index)
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
        cdvd.m_executed != 5u ||
        cdvd_kernel.cpu().state().m_gpr[9u] !=
            iop::c_hw::k_cdvd_ncmd_ready ||
        cdvd_kernel.cpu().state().m_gpr[10u] !=
            iop::c_hw::k_cdvd_no_disc)
    {
        return fail("Guest LBU did not read CDVD no-media state");
    }

    // Real ROM0 writes SB to CDVD interrupt-status 0xBF402008.
    // Test that the guest acknowledges outstanding reasons without
    // adding an IRQ, claiming a completed command or inventing media.
    const uint32_t cdvd_ack_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x35082008u, // ori t0,t0,0x2008
        0x240900ffu, // addiu t1,zero,0xff
        0xa1090000u, // sb t1,0(t0)
        0x910a0000u, // lbu t2,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        put32(rom, index * 4u, cdvd_ack_instructions[index]);
    }
    iop::c_kernel cdvd_ack_kernel{};
    if (!cdvd_ack_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 CDVD ack fixture failed to install ROM");
    }
    const iop::s_kernel_run_result ack =
        cdvd_ack_kernel.run(16u, 4u);
    if (ack.m_stop != iop::e_kernel_stop::breakpoint ||
        ack.m_executed != 5u ||
        cdvd_ack_kernel.cpu().state().m_gpr[10u] != 0u ||
        cdvd_ack_kernel.cpu().last_memory_fault().m_valid ||
        cdvd_ack_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest CDVD interrupt-ack created phantom IRQ/fault");
    }

    // Execute genuine R3000 SB/LBU instructions against CDVD DEC_SET
    // through the KSEG1 MMIO mirror. This tests register programming
    // only, not decryption or successful disc I/O.
    const uint32_t cdvd_dec_set_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x3508203au, // ori t0,t0,0x203a
        0x2409005au, // addiu t1,zero,0x5a
        0xa1090000u, // sb t1,0(t0)
        0x910a0000u, // lbu t2,0(t0)
        0xa1000000u, // sb zero,0(t0) (ROM0 reset store)
        0x910b0000u, // lbu t3,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        put32(rom, index * 4u, cdvd_dec_set_instructions[index]);
    }
    iop::c_kernel cdvd_dec_set_kernel{};
    if (!cdvd_dec_set_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 CDVD DEC_SET fixture failed to install ROM");
    }
    const iop::s_kernel_run_result dec_set =
        cdvd_dec_set_kernel.run(16u, 4u);
    if (dec_set.m_stop != iop::e_kernel_stop::breakpoint ||
        dec_set.m_executed != 7u ||
        cdvd_dec_set_kernel.cpu().state().m_gpr[10u] != 0x5au ||
        cdvd_dec_set_kernel.cpu().state().m_gpr[11u] != 0u ||
        cdvd_dec_set_kernel.cpu().last_memory_fault().m_valid ||
        cdvd_dec_set_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("Guest CDVD DEC_SET access faulted or fabricated I/O");
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
