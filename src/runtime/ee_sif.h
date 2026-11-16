#pragma once

#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
// EE-side SIF mailbox and kernel-maintained SIF system registers.
//
// The hardware mailbox registers are at 0x1000F200..0x1000F230;
// SIF_SYSREG_* (0x80000000..0x80000002) are kernel software state,
// never MMIO. Only the EE half of the interface is modeled here:
// there is no implicit IOP startup, DMA packet or interrupt.
class c_ee_sif
{
public:
    static constexpr uint32_t k_mscom_address = 0x1000f200u;
    static constexpr uint32_t k_smcom_address = 0x1000f210u;
    static constexpr uint32_t k_msflag_address = 0x1000f220u;
    static constexpr uint32_t k_smflag_address = 0x1000f230u;
    static constexpr uint32_t k_sysreg_base = 0x80000000u;

    void reset()
    {
        m_mscom = 0u;
        m_smcom = 0u;
        m_msflag = 0u;
        m_smflag = 0u;
        for (uint32_t& value : m_sysregs)
        {
            value = 0u;
        }
    }

    // SIF kernel syscall arguments: 1=MAINADDR, 2=SUBADDR,
    // 3=MSFLAG, 4=SMFLAG, or 0x80000000..0x80000002.
    // Unknown registers are deliberately not assigned fake state.
    bool get_reg(uint32_t id, uint32_t* result) const
    {
        if (result == nullptr)
        {
            return false;
        }

        switch (id)
        {
        case 1u: *result = m_mscom; return true;
        case 2u: *result = m_smcom; return true;
        case 3u: *result = m_msflag; return true;
        case 4u: *result = m_smflag; return true;
        case k_sysreg_base:
        case k_sysreg_base + 1u:
        case k_sysreg_base + 2u:
            *result = m_sysregs[id - k_sysreg_base];
            return true;
        default:
            return false;
        }
    }

    bool set_reg(uint32_t id, uint32_t value)
    {
        switch (id)
        {
        case 1u:
            // MSCOM: EE publishes its receive-buffer address to the IOP.
            m_mscom = value;
            return true;
        case 2u:
            // SMCOM is published by IOP, not writable by the EE.
            return false;
        case 3u:
            // EE raises MSFLAG bits. IOP clears them with its own mask.
            m_msflag |= value;
            return true;
        case 4u:
            // EE acknowledges IOP->EE SMFLAG bits by clearing its mask.
            m_smflag &= ~value;
            return true;
        case k_sysreg_base:
        case k_sysreg_base + 1u:
        case k_sysreg_base + 2u:
            m_sysregs[id - k_sysreg_base] = value;
            return true;
        default:
            return false;
        }
    }

    // Future IOP emulator / HLE producer hooks; never called implicitly
    // by the EE kernel service handlers.
    void publish_iop_subaddr(uint32_t value)
    {
        m_smcom = value;
    }

    void publish_iop_smflag(uint32_t bits)
    {
        m_smflag |= bits;
    }

    // The IOP acknowledges EE-originated MSFLAG bits. Only a real
    // IOP register write may call this; the EE SifSetReg path sets.
    void acknowledge_ee_msflag(uint32_t bits)
    {
        m_msflag &= ~bits;
    }

    static bool mmio_read(
        void* user, uint32_t address, uint32_t width, s_ee_u128* result
    )
    {
        if (user == nullptr || result == nullptr || width != 4u)
        {
            return false;
        }

        c_ee_sif* sif = static_cast<c_ee_sif*>(user);
        uint32_t id = 0u;
        if (!decode_mmio(address, &id))
        {
            return false;
        }

        uint32_t value = 0u;
        if (!sif->get_reg(id, &value))
        {
            return false;
        }

        *result = {value, 0u};
        return true;
    }

    static bool mmio_write(
        void* user, uint32_t address, uint32_t width,
        const s_ee_u128* value
    )
    {
        if (user == nullptr || value == nullptr || width != 4u)
        {
            return false;
        }

        c_ee_sif* sif = static_cast<c_ee_sif*>(user);
        uint32_t id = 0u;
        return decode_mmio(address, &id) &&
               sif->set_reg(id, static_cast<uint32_t>(value->m_lo));
    }

private:
    static bool decode_mmio(uint32_t address, uint32_t* id)
    {
        if (id == nullptr)
        {
            return false;
        }

        switch (address)
        {
        case k_mscom_address: *id = 1u; return true;
        case k_smcom_address: *id = 2u; return true;
        case k_msflag_address: *id = 3u; return true;
        case k_smflag_address: *id = 4u; return true;
        default:
            return false;
        }
    }

    uint32_t m_mscom = 0u;
    uint32_t m_smcom = 0u;
    uint32_t m_msflag = 0u;
    uint32_t m_smflag = 0u;
    uint32_t m_sysregs[3]{};
};
} // namespace ee
