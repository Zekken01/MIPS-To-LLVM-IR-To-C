#include "iop_sifman_bridge.h"

namespace iop
{
namespace
{
bool create_stub(
    c_cpu& cpu, uint32_t pc, uint32_t service
)
{
    // MIPS I: lui v0,upper; ori v0,v0,lower; syscall; jr ra; nop
    // The HLE operation is only performed if an IOP thread actually
    // reaches this syscall after calling its resolved IRX import.
    const uint32_t code[] =
    {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0x00000000u
    };
    bool valid = true;
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        valid = valid && cpu.write32(pc + i * 4u, code[i]);
    }
    return valid;
}
} // namespace

c_sifman_bridge::c_sifman_bridge(
    c_kernel& kernel, ee::c_ee_sif& sif
)
    : m_kernel(kernel), m_sif(sif)
{
}

bool c_sifman_bridge::install()
{
    // Validate before writing stubs. Duplicate library registrations
    // are rejected rather than silently overwriting an IOP library.
    if (!m_kernel.register_library("sifman", 23u, k_get_smflag_stub))
    {
        return false;
    }
    if (!m_kernel.register_library("sifman", 24u, k_set_smflag_stub))
    {
        return false;
    }
    return create_stub(
        m_kernel.cpu(), k_get_smflag_stub, k_get_smflag_syscall
    ) && create_stub(
        m_kernel.cpu(), k_set_smflag_stub, k_set_smflag_syscall
    );
}

bool c_sifman_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    switch (service)
    {
    case k_get_smflag_syscall:
    {
        uint32_t value = 0u;
        if (!m_sif.get_reg(4u, &value))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = value;
        return true;
    }

    case k_set_smflag_syscall:
        // Only the IOP owns the producer side of SMFLAG. No value
        // is published until a guest R3000 thread invokes this stub.
        m_sif.publish_iop_smflag(
            kernel.cpu().state().m_gpr[4]
        );
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    default:
        return false;
    }
}
} // namespace iop
