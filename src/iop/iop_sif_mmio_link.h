#pragma once

#include "iop_hw.h"
#include "ee_sif.h"

#include <cstdint>

namespace iop
{
// Translate genuine IOP SIF MMIO into the single EE hardware register
// object. Neither side gets a synthetic readiness bit or DMA completion.
class c_sif_mmio_link
{
public:
    static void bind(c_hw& iop_hardware, ee::c_ee_sif& ee_sif)
    {
        iop_hardware.bind_sif_mmio(
            &ee_sif, &c_sif_mmio_link::read, &c_sif_mmio_link::write
        );
    }

    static bool read(
        void* user, uint32_t address, uint32_t width, uint32_t* result
    )
    {
        bool success = false;
        if (user != nullptr && result != nullptr && width == 4u)
        {
            uint32_t id = 0u;
            if (decode(address, &id))
            {
                const ee::c_ee_sif* sif =
                    static_cast<const ee::c_ee_sif*>(user);
                success = sif->get_reg(id, result);
            }
        }
        return success;
    }

    static bool write(
        void* user, uint32_t address, uint32_t width, uint32_t value
    )
    {
        bool success = false;
        if (user != nullptr && width == 4u)
        {
            ee::c_ee_sif* sif = static_cast<ee::c_ee_sif*>(user);
            switch (address)
            {
            case c_hw::k_sif_smcom:
                sif->publish_iop_subaddr(value);
                success = true;
                break;
            case c_hw::k_sif_msflag:
                sif->acknowledge_ee_msflag(value);
                success = true;
                break;
            case c_hw::k_sif_smflag:
                sif->publish_iop_smflag(value);
                success = true;
                break;
            default:
                // MSCOM belongs to the EE; other guest addresses
                // cannot silently mutate EE SIF hardware state.
                break;
            }
        }
        return success;
    }

private:
    static bool decode(uint32_t address, uint32_t* id)
    {
        bool success = true;
        if (id == nullptr)
        {
            success = false;
        }
        else
        {
            switch (address)
            {
            case c_hw::k_sif_mscom: *id = 1u; break;
            case c_hw::k_sif_smcom: *id = 2u; break;
            case c_hw::k_sif_msflag: *id = 3u; break;
            case c_hw::k_sif_smflag: *id = 4u; break;
            default: success = false; break;
            }
        }
        return success;
    }
};
} // namespace iop
