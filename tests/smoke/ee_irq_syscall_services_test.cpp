#include "ee_irq_syscall_services.h"

#include <cstdint>
#include <cstdio>

namespace
{
bool check(const char* label, int32_t signed_number, bool irq_active,
           uint32_t expected_number, bool expected_interrupt_safe)
{
    const ee_irq_syscall::s_decoded_service actual =
        ee_irq_syscall::decode(signed_number, irq_active);
    const bool passed =
        actual.m_number == expected_number &&
        actual.m_interrupt_safe == expected_interrupt_safe;
    if (!passed)
    {
        std::fprintf(stderr,
            "%s: number=0x%08x safe=%u expected=0x%08x safe=%u\n",
            label, actual.m_number, actual.m_interrupt_safe ? 1u : 0u,
            expected_number, expected_interrupt_safe ? 1u : 0u);
    }
    return passed;
}
}

int main()
{
    bool passed = true;
    passed = check("retail IRQ iReferThreadStatus", -0x31, true,
                   0x31u, true) && passed;
    passed = check("retail IRQ _iGetThreadId", -0x2f, true,
                   0x2fu, true) && passed;
    passed = check("_iGetThreadId outside IRQ", -0x2f, false,
                   static_cast<uint32_t>(-0x2f), false) && passed;
    passed = check("normal GetThreadId", 0x2f, true,
                   0x2fu, false) && passed;
    passed = check("negative outside IRQ", -0x31, false,
                   static_cast<uint32_t>(-0x31), false) && passed;
    passed = check("normal ReferThreadStatus", 0x30, false,
                   0x30u, false) && passed;
    passed = check("normal iReferThreadStatus", 0x31, false,
                   0x31u, false) && passed;
    passed = check("unknown negative in IRQ", -0x38, true,
                   static_cast<uint32_t>(-0x38), false) && passed;
    passed = check("unknown syscall in IRQ", -0x7d, true,
                   static_cast<uint32_t>(-0x7d), false) && passed;
    passed = check("sif IRQ unchanged", -0x78, true,
                   0x78u, true) && passed;
    passed = check("interrupt wakeup unchanged", -0x34, true,
                   0x34u, true) && passed;
    passed = check("interrupt semaphore unchanged", -0x43, true,
                   0x43u, true) && passed;
    if (passed)
    {
        std::puts("ee_irq_syscall_services: PASS");
    }
    return passed ? 0 : 1;
}
