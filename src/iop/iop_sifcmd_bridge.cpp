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

    m_installed = create_stub(
        m_kernel.cpu(), k_init_cmd_stub, k_init_cmd_syscall
    ) && create_stub(
        m_kernel.cpu(), k_get_sreg_stub, k_get_sreg_syscall
    ) && create_stub(
        m_kernel.cpu(), k_set_sreg_stub, k_set_sreg_syscall
    );
    return m_installed;
}

bool c_sifcmd_bridge::initialize_hle_bootstrap()
{
    if (!m_installed || m_initialized || m_hle_bootstrapped)
    {
        return false;
    }

    // SIF_STAT_CMDINIT | SIF_STAT_BOOTEND. The user's ps2-web-recomp
    // SDBZ HLE IOP kernel publishes precisely 0x60000 at boot, as its
    // IOP services are host-backed. This is deliberately NOT a guest
    // R3000 signal, real ROM0 readiness, or an RPC completion. Only the
    // explicit HLE runtime initializer may establish this service state.
    m_initialized = true;
    m_hle_bootstrapped = true;
    m_sif.publish_iop_subaddr(k_command_buffer);
    m_sif.publish_iop_smflag(0x00060000u);
    return true;
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
    // The EE initialization and CHANGE_SADDR commands carry one word
    // after the 16-byte SIFCMD header; SET_SREG carries two words.
    // DMA transfers round to 16 bytes, but the header records the
    // original 20/24-byte packet size.
    if (!m_initialized || command == nullptr ||
        (size != 16u && size != 20u && size != 24u) ||
        words.size() < size / 4u ||
        (words[0] & 0xffu) != size ||
        words[1] != 0u)
    {
        return false;
    }

    // EE's 16-byte INIT_CMD with opt=1 requests an IOP->EE
    // SET_SREG command (SREG[0] = 1). ps2-web-recomp/SDBZ
    // sends that packet to the EE through the SIF0 command receiver.
    // This is not the 20-byte receive-address publication, and it
    // must never update an EE SREG without an actual reply packet.
    const uint32_t cid = words[2];
    if (cid == 0x80000002u && size == 16u &&
        words[3] == 1u)
    {
        *command = {cid, 0u, 0u, true};
        return true;
    }

    // Every remaining command needs at least the fifth word.
    // Only the explicitly handled opt=1 handshake is 16 bytes.
    if (size == 16u || words[3] != 0u)
    {
        return false;
    }

    // The IOP consumes only validated command packets. Neither a
    // published address nor a DMA flag synthesizes an RPC reply.
    const uint32_t index_or_address = words[4];
    if (cid == 0x80000001u && size == 24u &&
        index_or_address < 32u)
    {
        *command = {cid, index_or_address, words[5]};
        return true;
    }

    // 0x80000000 = CHANGE_SADDR, 0x80000002 = INIT_CMD.
    // Keep the already-tested 24-byte INIT_CMD variant only when
    // its extra word is zero, while accepting the real 20-byte form.
    const bool receive_command =
        (cid == 0x80000000u && size == 20u) ||
        (cid == 0x80000002u &&
         (size == 20u || (size == 24u && words[5] == 0u)));
    if (receive_command &&
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
    else if ((command.m_cid == 0x80000000u ||
              command.m_cid == 0x80000002u) &&
             !command.m_request_sreg_reply)
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
