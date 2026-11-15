#include "ee_intc.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* error)
{
    std::fprintf(stderr, "EE INTC registers: %s\n", error);
    return 1;
}
} // namespace

int main()
{
    ee::s_ee_cpu_state cpu{};
    ee::ee_memory memory(4096);
    ee::c_ee_intc intc{};
    intc.bind_cpu(&cpu);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &intc;
    runtime.m_mmio_read = &ee::c_ee_intc::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_intc::mmio_write;

    constexpr uint32_t stat = ee::c_ee_intc::k_stat_address;
    constexpr uint32_t mask = ee::c_ee_intc::k_mask_address;
    constexpr uint32_t line = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC);
    constexpr uint32_t cause2 = 1u << 2;
    constexpr uint32_t cause3 = 1u << 3;

    cpu.m_cop0[ee::COP0_Status] = ee::STATUS_IE | ee::STATUS_EIE | line;
    if (ee::ee_mem_read32(&runtime, stat) != 0 ||
        ee::ee_mem_read32(&runtime, mask) != 0 ||
        intc.pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & line) != 0)
    {
        return fail("reset state not empty");
    }

    if (!intc.raise(2u) || intc.raise(32u) ||
        intc.status() != cause2 || intc.pending() != 0 ||
        ee::ee_mem_read32(&runtime, stat) != cause2 ||
        ee::ee_interrupts_enabled(&cpu))
    {
        return fail("raising a masked or invalid interrupt");
    }

    // INTC_MASK is write-one-to-toggle. Enabling the pending source must
    // assert EE COP0 Cause.IP2; disabling it must deassert the line.
    ee::ee_mem_write32(&runtime, mask, cause2);
    if (intc.mask() != cause2 || intc.pending() != cause2 ||
        ee::ee_mem_read32(&runtime, mask) != cause2 ||
        (cpu.m_cop0[ee::COP0_Cause] & line) == 0 ||
        !ee::ee_interrupts_enabled(&cpu))
    {
        return fail("mask toggle did not assert pending IP2");
    }

    ee::ee_mem_write32(&runtime, mask, cause2);
    if (intc.mask() != 0 || intc.status() != cause2 ||
        intc.pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & line) != 0 ||
        ee::ee_interrupts_enabled(&cpu))
    {
        return fail("second mask toggle did not deassert IP2");
    }

    // Acknowledge only the bits written to INTC_STAT.
    if (!intc.raise(3u))
    {
        return fail("raising second interrupt");
    }
    ee::ee_mem_write32(&runtime, mask, cause2 | cause3);
    ee::ee_mem_write32(&runtime, stat, cause2);
    if (ee::ee_mem_read32(&runtime, stat) != cause3 ||
        ee::ee_mem_read32(&runtime, mask) != (cause2 | cause3) ||
        intc.pending() != cause3 ||
        (cpu.m_cop0[ee::COP0_Cause] & line) == 0)
    {
        return fail("acknowledge cleared unrequested bit or lost pending IP2");
    }

    // Direct physical address aliases must reach the same INTC instance.
    memory.set_kseg_ram_aliases(true);
    ee::ee_mem_write32(&runtime, 0xb000f000u, cause3);
    if (ee::ee_mem_read32(&runtime, 0x9000f000u) != 0 ||
        intc.pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & line) != 0)
    {
        return fail("KSEG physical INTC register alias");
    }

    // Unimplemented widths and other device addresses fail closed.
    intc.raise(2u);
    ee::ee_mem_write8(&runtime, stat, 0xffu);
    ee::ee_mem_write16(&runtime, mask, 0xffffu);
    ee::ee_mem_write64(&runtime, stat, ~uint64_t{0});
    ee::s_ee_u128 quad{};
    quad.m_lo = ~uint64_t{0};
    quad.m_hi = ~uint64_t{0};
    ee::ee_mem_write128(&runtime, mask, &quad);
    ee::ee_mem_read128(&runtime, stat, &quad);
    if (intc.status() != cause2 || intc.mask() != (cause2 | cause3) ||
        intc.pending() != cause2 ||
        ee::ee_mem_read8(&runtime, stat) != 0 ||
        ee::ee_mem_read16(&runtime, mask) != 0 ||
        ee::ee_mem_read64(&runtime, stat) != 0 ||
        quad.m_lo != 0 || quad.m_hi != 0)
    {
        return fail("unimplemented widths changed device registers");
    }

    ee::ee_mem_write32(&runtime, 0x1000f020u, 0xffffffffu);
    if (ee::ee_mem_read32(&runtime, 0x1000f020u) != 0 ||
        intc.status() != cause2 || intc.mask() != (cause2 | cause3))
    {
        return fail("unimplemented register fabricated state");
    }

    // Clearing an active source must immediately clear Cause.IP2.
    ee::ee_mem_write32(&runtime, stat, cause2);
    if (intc.status() != 0 || intc.pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & line) != 0)
    {
        return fail("status acknowledge did not lower IP2");
    }

    ee::s_ee_cpu_state replacement_cpu{};
    intc.raise(3u);
    intc.bind_cpu(&replacement_cpu);
    if ((cpu.m_cop0[ee::COP0_Cause] & line) != 0 ||
        (replacement_cpu.m_cop0[ee::COP0_Cause] & line) == 0)
    {
        return fail("CPU rebind left stale interrupt line");
    }

    intc.reset();
    if (intc.status() != 0 || intc.mask() != 0 ||
        intc.pending() != 0 ||
        (replacement_cpu.m_cop0[ee::COP0_Cause] & line) != 0)
    {
        return fail("reset did not clear state and interrupt line");
    }

    std::puts("EE INTC status/mask/ack/IP2: PASS");
    return 0;
}
