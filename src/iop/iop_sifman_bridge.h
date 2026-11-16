#pragma once

#include "iop_kernel.h"
#include "ee_sif.h"

#include <cstdint>

namespace iop
{
// A minimal, explicit IOP SIFMAN high-level service implementation.
//
// Installing the service registers two concrete guest-callable R3000 stubs,
// but does NOT publish any SIF mailbox, readiness bit, or RPC response.
// An IOP thread has to execute the import stub for the register to change.
//
// Stubs occupy the otherwise reserved low IOP kernel area, outside the
// allocator's 0x20000.. RAM region. Other HLE kernel services must use
// distinct reserved addresses and syscall identifiers.
class c_sifman_bridge
{
public:
    static constexpr uint32_t k_get_smflag_stub = 0x00018000u;
    static constexpr uint32_t k_set_smflag_stub = 0x00018020u;
    static constexpr uint32_t k_get_smflag_syscall = 0x494f0017u;
    static constexpr uint32_t k_set_smflag_syscall = 0x494f0018u;

    c_sifman_bridge(c_kernel& kernel, ee::c_ee_sif& sif);

    // Registers sifman ordinals 23 and 24 as executable guest-callable
    // import destinations. Returns false instead of overriding exports.
    bool install();

    // Can be composed with other kernel syscall handlers. Unknown services
    // are NOT acknowledged or assigned invented return values.
    bool handle_syscall(c_kernel& kernel, uint32_t service);

private:
    c_kernel& m_kernel;
    ee::c_ee_sif& m_sif;
};
} // namespace iop
