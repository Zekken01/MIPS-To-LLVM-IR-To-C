#pragma once

#include <cstdint>

// Only known interrupt-safe EE syscalls may use their negative kernel
// encodings, and only during an actual registered guest IRQ callback.
namespace ee_irq_syscall
{

struct s_decoded_service
{
    uint32_t m_number = 0u;
    bool m_interrupt_safe = false;
};

inline s_decoded_service decode(int32_t signed_number, bool irq_active)
{
    s_decoded_service result{
        static_cast<uint32_t>(signed_number), false
    };
    if (irq_active)
    {
        switch (signed_number)
        {
        case -0x2e: // iReleaseWaitThread
        case -0x31: // iReferThreadStatus
        case -0x34: // iWakeupThread
        case -0x36: // iCancelWakeupThread
        case -0x43: // iSignalSema
        case -0x68: // iFlushCache
        case -0x76: // iSifSetDma
        case -0x77: // iSifDmaStat
        case -0x78: // iSifSetDChain
            result.m_number = static_cast<uint32_t>(-signed_number);
            result.m_interrupt_safe = true;
            break;
        default:
            break;
        }
    }
    return result;
}

} // namespace ee_irq_syscall
