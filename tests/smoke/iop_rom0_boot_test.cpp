#include "iop_cpu.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP ROM0 boot: %s\n", message);
    return 1;
}

void word(std::vector<uint8_t>& rom, uint32_t offset, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        rom[offset + i] =
            static_cast<uint8_t>(value >> (i * 8u));
    }
}
} // namespace

int main()
{
    // Tiny original test instructions (not copyrighted BIOS data).
    // They execute from the *real* BIOS reset vector, fetch physical
    // ROM, inspect CP0 reset state and write IOP RAM.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    word(rom, 0u, 0x3c081fc0u); // lui t0,0x1fc0
    word(rom, 4u, 0x8d090000u); // lw t1,0(t0)
    word(rom, 8u, 0x400a7800u); // mfc0 t2,$15 PRid
    word(rom, 12u, 0x400b6000u); // mfc0 t3,$12 Status
    word(rom, 16u, 0x240c0009u); // addiu t4,zero,9
    word(rom, 20u, 0xac0c0100u); // sw t4,0x100(zero)
    word(rom, 24u, 0x0000000du); // break

    iop::c_cpu cpu;
    const std::vector<uint8_t> invalid(100u, 0u);
    if (cpu.install_rom0(invalid) ||
        cpu.reset_rom0() ||
        !cpu.install_rom0(rom) ||
        !cpu.has_rom0() ||
        !cpu.reset_rom0())
    {
        return fail("ROM0 size check or boot-reset validation");
    }

    uint32_t actual = 0u;
    if (cpu.state().m_pc != iop::c_cpu::k_rom0_reset_pc ||
        cpu.state().m_next_pc !=
            iop::c_cpu::k_rom0_reset_pc + 4u ||
        cpu.state().m_cop0_status != 0x10900000u ||
        cpu.state().m_cop0_prid != 0x1fu ||
        !cpu.read32(0x1fc00000u, &actual) ||
        actual != 0x3c081fc0u ||
        !cpu.read32(0x9fc00000u, &actual) ||
        actual != 0x3c081fc0u ||
        !cpu.read32(0xbfc00000u, &actual) ||
        actual != 0x3c081fc0u ||
        cpu.write32(0xbfc00000u, 0xffffffffu) ||
        cpu.write32(0x1fc00000u, 0xffffffffu) ||
        cpu.read32(0x1fbffffcu, &actual) ||
        cpu.read32(0xc0000000u, &actual))
    {
        return fail("ROM0 physical/KSEG mapping or read-only isolation");
    }

    const iop::s_run_result ran = cpu.run(16u);
    if (ran.m_result != iop::e_step_result::breakpoint ||
        ran.m_instructions != 6u ||
        ran.m_pc != 0xbfc00018u ||
        cpu.state().m_gpr[9] != 0x3c081fc0u ||
        cpu.state().m_gpr[10] != 0x1fu ||
        cpu.state().m_gpr[11] != 0x10900000u ||
        !cpu.read32(0x100u, &actual) || actual != 9u)
    {
        return fail("real ROM reset-vector R3000 instruction execution");
    }
    if (!cpu.reset_rom0() || cpu.cycles() != 0u ||
        cpu.state().m_pc != 0xbfc00000u ||
        !cpu.read32(0x100u, &actual) || actual != 0u ||
        !cpu.read32(0xbfc00000u, &actual) ||
        actual != 0x3c081fc0u)
    {
        return fail("guest RAM resets without altering ROM0");
    }

    // The existing kernel scheduler executes the ROM bootstrap
    // without manufacturing a module or a boot thread.
    iop::c_kernel kernel;
    if (kernel.start_rom0_boot(invalid) ||
        !kernel.start_rom0_boot(rom) ||
        !kernel.rom0_boot_active() ||
        kernel.start_rom0_boot(rom) ||
        kernel.current_thread_id() != 0u)
    {
        return fail("ROM0 kernel startup should be opt-in, exclusive");
    }
    iop::s_kernel_run_result result = kernel.run(3u, 1u);
    if (result.m_stop != iop::e_kernel_stop::budget_exhausted ||
        result.m_executed != 3u ||
        kernel.cycles() != 3u ||
        kernel.cpu().state().m_pc != 0xbfc0000cu)
    {
        return fail("ROM0 execution budget or persistent state");
    }
    result = kernel.run(8u, 2u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_executed != 3u ||
        result.m_guest_pc != 0xbfc00018u ||
        result.m_thread_id != 0u ||
        kernel.cycles() != 6u ||
        kernel.module(1u) != nullptr)
    {
        return fail("ROM0 halted at wrong guest address or invented module");
    }

    kernel.reset();
    if (kernel.rom0_boot_active() ||
        !kernel.start_rom0_boot(rom))
    {
        return fail("IOP reset failed to clear ROM0 boot mode");
    }
    result = kernel.run(1u);
    if (result.m_stop != iop::e_kernel_stop::budget_exhausted ||
        result.m_executed != 1u)
    {
        return fail("ROM0 restart is not deterministic");
    }

    iop::c_kernel has_module;
    if (!has_module.register_library("SYSBOOT", 1u, 0x20000u) ||
        has_module.start_rom0_boot(rom))
    {
        return fail("ROM0 boot attempted over installed IRX imports");
    }

    // A real BIOS will access IOP MMIO. Until hardware is implemented
    // these reads must fault, not alias other RAM or silently continue.
    std::vector<uint8_t> mmio_rom = rom;
    word(mmio_rom, 0u, 0x3c081f80u); // lui t0,0x1f80
    word(mmio_rom, 4u, 0x8d091000u); // lw t1,0x1000(t0)
    if (!cpu.install_rom0(mmio_rom) ||
        !cpu.reset_rom0())
    {
        return fail("IOP MMIO test image could not reset");
    }
    const iop::s_run_result fault = cpu.run(8u);
    if (fault.m_result != iop::e_step_result::memory_fault ||
        fault.m_instructions != 1u ||
        fault.m_pc != 0xbfc00004u ||
        cpu.state().m_pc != 0xbfc00004u)
    {
        return fail("unimplemented IOP MMIO was silently accepted");
    }

    std::printf("IOP ROM0 boot: PASS\n");
    return 0;
}
