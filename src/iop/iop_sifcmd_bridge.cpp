#include "iop_sifcmd_bridge.h"

namespace iop
{
namespace
{
bool create_stub(c_cpu& cpu, uint32_t pc, uint32_t service)
{
    // The guest reaches a genuine R3000 syscall via its relocated import.
    const uint32_t instructions[5] =
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
        valid = cpu.write32(pc + i * 4u, instructions[i]) && valid;
    }
    return valid;
}
} // namespace

c_sifcmd_bridge::c_sifcmd_bridge(
    c_kernel& kernel, ee::c_ee_sif& sif
)
    : m_kernel(kernel), m_sif(sif)
{
}

bool c_sifcmd_bridge::install()
{
    // These ordinals match the IOP sifcmd exports. Do not register
    // unimplemented RPC or guest callback services as successful stubs.
    if (!m_kernel.register_library("sifcmd", 4u, k_init_cmd_stub) ||
        !m_kernel.register_library("sifcmd", 6u, k_get_sreg_stub) ||
        !m_kernel.register_library("sifcmd", 7u, k_set_sreg_stub))
    {
        return false;
    }

    return create_stub(
        m_kernel.cpu(), k_init_cmd_stub, k_init_cmd_syscall
    ) && create_stub(
        m_kernel.cpu(), k_get_sreg_stub, k_get_sreg_syscall
    ) && create_stub(
        m_kernel.cpu(), k_set_sreg_stub, k_set_sreg_syscall
    );
}

bool c_sifcmd_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    switch (service)
    {
    case k_init_cmd_syscall:
        // The IOP publishes SUBADDR only after its R3000 guest code
        // has invoked sceSifInitCmd; never publish the SMFLAG here.
        m_initialized = true;
        m_sif.publish_iop_subaddr(k_command_buffer);
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    case k_get_sreg_syscall:
        if (!m_initialized)
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] =
            m_sregs[kernel.cpu().state().m_gpr[4] & 31u];
        return true;

    case k_set_sreg_syscall:
        if (!m_initialized)
        {
            return false;
        }
        m_sregs[kernel.cpu().state().m_gpr[4] & 31u] =
            kernel.cpu().state().m_gpr[5];
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    default:
        return false;
    }
}

bool c_sifcmd_bridge::initialized() const
{
    return m_initialized;
}

bool c_sifcmd_bridge::prepare_command(
    const std::vector<uint32_t>& words,
    uint32_t size,
    s_command* command
) const
{
    if (!m_initialized || command == nullptr ||
        size != 24u || words.size() < 6u ||
        (words[0] & 0xffu) != 24u ||
        words[1] != 0u)
    {
        return false;
    }

    // The IOP consumes these SIF command packet headers directly;
    // no host-synthesized command, readiness or return packet exists.
    // CID 0x80000001 carries an SREG index/value.
    // CID 0x80000002 publishes the EE command receive buffer (opt=0).
    const uint32_t cid = words[2];
    const uint32_t opt = words[3];
    const uint32_t index_or_address = words[4];
    if (cid == 0x80000001u && opt == 0u &&
        index_or_address < 32u)
    {
        *command = {cid, index_or_address, words[5]};
        return true;
    }

    if (cid == 0x80000002u && opt == 0u &&
        index_or_address != 0u &&
        (index_or_address & 0x0fu) == 0u &&
        index_or_address < 0x02000000u)
    {
        *command = {cid, 0u, index_or_address};
        return true;
    }

    // Other CIDs require registered guest callback handlers and
    // reply scheduling. Reject them until that path is implemented.
    return false;
}

void c_sifcmd_bridge::commit_command(const s_command& command)
{
    if (command.m_cid == 0x80000001u)
    {
        m_sregs[command.m_index] = command.m_value;
    }
    else if (command.m_cid == 0x80000002u)
    {
        m_ee_receive_address = command.m_value;
    }
}

uint32_t c_sifcmd_bridge::sreg(uint32_t index) const
{
    return index < 32u ? m_sregs[index] : 0u;
}

uint32_t c_sifcmd_bridge::ee_receive_address() const
{
    return m_ee_receive_address;
}
} // namespace iop
