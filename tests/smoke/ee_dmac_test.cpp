#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* text)
{
    std::fprintf(stderr, "EE DMAC status: %s\n", text);
    return 1;
}
} // namespace

int main()
{
    ee::s_ee_cpu_state cpu{};
    ee::ee_memory ram(4096);
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &ram;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    constexpr uint32_t d_stat = ee::c_ee_dmac::k_stat_address;
    constexpr uint32_t i_stat = ee::c_ee_intc::k_stat_address;
    constexpr uint32_t i_mask = ee::c_ee_intc::k_mask_address;
    constexpr uint32_t dmac_ip = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);
    constexpr uint32_t intc_ip = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC);
    constexpr uint32_t ch1 = 1u << 1;
    constexpr uint32_t ch8 = 1u << 8;

    cpu.m_cop0[ee::COP0_Status] = ee::STATUS_IE | ee::STATUS_EIE |
                                   dmac_ip | intc_ip;
    if (ee::ee_mem_read32(&runtime, d_stat) != 0 ||
        hardware.dmac().pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) != 0)
    {
        return fail("reset DMAC status or CPU cause");
    }

    if (!hardware.dmac().raise(1u) || hardware.dmac().raise(10u) ||
        hardware.dmac().status() != ch1 ||
        hardware.dmac().pending() != 0 ||
        ee::ee_interrupts_enabled(&cpu))
    {
        return fail("unmasked completion raised an interrupt");
    }

    // D_STAT upper half toggles masks, not overwrites them.
    ee::ee_mem_write32(&runtime, d_stat, ch1 << 16u);
    if (ee::ee_mem_read32(&runtime, d_stat) != (ch1 | (ch1 << 16u)) ||
        hardware.dmac().pending() != ch1 ||
        (cpu.m_cop0[ee::COP0_Cause] & dmac_ip) == 0 ||
        !ee::ee_interrupts_enabled(&cpu))
    {
        return fail("enabling masked pending DMA completion");
    }

    // Direct physical aliases are opt-in; the fixture must enable them
    // before touching the KSEG1 address.
    ram.set_kseg_ram_aliases(true);
    ee::ee_mem_write32(&runtime, 0xb000e010u, ch1 << 16u);
    if (hardware.dmac().status() != ch1 ||
        hardware.dmac().pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & dmac_ip) != 0)
    {
        return fail("DMA mask did not toggle off through KSEG1 alias");
    }

    // Acknowledge source while keeping a different source pending.
    hardware.dmac().raise(8u);
    ee::ee_mem_write32(&runtime, d_stat, (ch1 | ch8) << 16u);
    ee::ee_mem_write32(&runtime, d_stat, ch1);
    if (ee::ee_mem_read32(&runtime, d_stat) !=
            (ch8 | ((ch1 | ch8) << 16u)) ||
        hardware.dmac().pending() != ch8 ||
        (cpu.m_cop0[ee::COP0_Cause] & dmac_ip) == 0)
    {
        return fail("DMA write-one-to-clear status or independent sources");
    }

    // INTC and DMAC must share the bus but use different COP0 IP bits.
    hardware.intc().raise(2);
    ee::ee_mem_write32(&runtime, i_mask, 1u << 2);
    if (ee::ee_mem_read32(&runtime, i_stat) != 1u << 2 ||
        (cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) !=
            (dmac_ip | intc_ip))
    {
        return fail("DMAC/INTC bus dispatch or simultaneous pending bits");
    }

    ee::ee_mem_write32(&runtime, d_stat, ch8);
    if ((cpu.m_cop0[ee::COP0_Cause] & dmac_ip) != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & intc_ip) == 0)
    {
        return fail("DMA acknowledgement cleared unrelated INTC interrupt");
    }
    ee::ee_mem_write32(&runtime, i_stat, 1u << 2);
    if ((cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) != 0)
    {
        return fail("INTC acknowledgement did not clear its separate line");
    }

    // Register writes only: there is no DMA engine, QWC decrement, or
    // fabricated completion when a channel control register is written.
    ee::ee_mem_write32(&runtime, 0x10009000u, 0x100u);
    ee::ee_mem_write32(&runtime, 0x10009020u, 0x10u);
    if (ee::ee_mem_read32(&runtime, 0x10009000u) != 0 ||
        ee::ee_mem_read32(&runtime, 0x10009020u) != 0 ||
        hardware.dmac().status() != ((ch1 | ch8) << 16u))
    {
        return fail("unimplemented DMA channel control reported false state");
    }

    // Only 32-bit D_STAT accesses are supported at this milestone.
    ee::ee_mem_write8(&runtime, d_stat, 0xffu);
    ee::ee_mem_write16(&runtime, d_stat, 0xffffu);
    ee::ee_mem_write64(&runtime, d_stat, ~uint64_t{0});
    ee::s_ee_u128 all_ones{~uint64_t{0}, ~uint64_t{0}};
    ee::ee_mem_write128(&runtime, d_stat, &all_ones);
    ee::ee_mem_read128(&runtime, d_stat, &all_ones);
    if (hardware.dmac().status() != ((ch1 | ch8) << 16u) ||
        ee::ee_mem_read8(&runtime, d_stat) != 0 ||
        ee::ee_mem_read16(&runtime, d_stat) != 0 ||
        ee::ee_mem_read64(&runtime, d_stat) != 0 ||
        all_ones.m_lo != 0 || all_ones.m_hi != 0)
    {
        return fail("unsupported access widths mutated D_STAT");
    }

    if (!hardware.dmac().acknowledge(1u) ||
        hardware.dmac().acknowledge(10u))
    {
        return fail("DMA channel ack validation");
    }

    ee::s_ee_cpu_state next_cpu{};
    hardware.dmac().raise(8);
    hardware.bind_cpu(&next_cpu);
    if ((cpu.m_cop0[ee::COP0_Cause] & dmac_ip) != 0 ||
        (next_cpu.m_cop0[ee::COP0_Cause] & dmac_ip) == 0)
    {
        return fail("CPU rebind did not migrate the pending DMA IP3 bit");
    }

    hardware.reset();
    if (hardware.dmac().status() != 0 ||
        hardware.dmac().pending() != 0 ||
        hardware.intc().status() != 0 ||
        (next_cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) != 0)
    {
        return fail("hardware reset did not release pending interrupts");
    }

    std::puts("EE D_STAT / status mask / COP0 IP3: PASS");
    return 0;
}
