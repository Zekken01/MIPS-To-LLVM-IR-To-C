#pragma once

#include "iop_kernel.h"
#include "ee_sif.h"

#include <cstdint>
#include <vector>

namespace iop
{
// An explicit, guest-initialized IOP SIFCMD endpoint. The EE may deliver
// implemented system commands through its SIF1 DMA bridge. This does not
// create an IOP BIOS, synthesize SMFLAG, invoke RPC or acknowledge unknown CIDs.
class c_sifcmd_bridge
{
public:
    static constexpr uint32_t k_command_buffer = 0x0001e000u;
    static constexpr uint32_t k_init_cmd_stub = 0x00018100u;
    static constexpr uint32_t k_get_sreg_stub = 0x00018120u;
    static constexpr uint32_t k_set_sreg_stub = 0x00018140u;
    static constexpr uint32_t k_init_cmd_syscall = 0x494f0104u;
    static constexpr uint32_t k_get_sreg_syscall = 0x494f0106u;
    static constexpr uint32_t k_set_sreg_syscall = 0x494f0107u;

    struct s_command
    {
        uint32_t m_cid = 0u;
        uint32_t m_index = 0u;
        uint32_t m_value = 0u;
    };

    c_sifcmd_bridge(c_kernel& kernel, ee::c_ee_sif& sif);

    bool install();
    bool handle_syscall(c_kernel& kernel, uint32_t service);

    bool initialized() const;

    // Validation has no side effects, allowing the SIF1 bridge to
    // reject a whole descriptor batch transactionally. Words include
    // rounded DMA padding; size is the original unrounded packet size.
    bool prepare_command(
        const std::vector<uint32_t>& words,
        uint32_t size,
        s_command* command
    ) const;

    // Called only after the entire DMA descriptor batch has committed.
    void commit_command(const s_command& command);

    uint32_t sreg(uint32_t index) const;
    uint32_t ee_receive_address() const;

private:
    c_kernel& m_kernel;
    ee::c_ee_sif& m_sif;
    uint32_t m_sregs[32]{};
    uint32_t m_ee_receive_address = 0u;
    bool m_initialized = false;
};
} // namespace iop
