#pragma once

#include "iop_disc_loader.h"
#include "iop_sifcmd_bridge.h"
#include "ee_dmac.h"
#include "ee_memory.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace iop
{
// Bounded, explicit LOADFILE RPC server connected to guest-initialized
// IOP SIFCMD. Only actual EE SIF1 bind/call packets invoke this service.
// Outgoing replies require an EE-provided receive buffer and armed SIF0.
// No IOP firmware, SMFLAG readiness, or generic RPC dispatch is synthesized.
class c_sif_rpc_bridge
{
public:
    static constexpr uint32_t k_rpc_data_destination = 0xdeadbef0u;
    static constexpr uint32_t k_loadfile_server = 0x80000006u;
    static constexpr uint32_t k_bind_cid = 0x80000009u;
    static constexpr uint32_t k_call_cid = 0x8000000au;
    static constexpr uint32_t k_reply_cid = 0x80000008u;
    static constexpr uint32_t k_server_xor = 0xacacacacu;

    struct s_request
    {
        uint32_t m_words[14]{};
        uint32_t m_word_count = 0u;
        uint32_t m_result_destination = 0u;
        std::string m_guest_path;
    };

    c_sif_rpc_bridge(
        c_disc_loader& loader,
        c_sifcmd_bridge& sifcmd,
        ee::ee_memory& ee_ram,
        ee::c_ee_dmac& dmac
    );

    // No side effects: called during validation of the complete SIF1
    // descriptor list. Payload is EE RAM from the special SIF data DMA,
    // not an arbitrary host string or static ELF filename.
    bool prepare_command(
        const std::vector<uint32_t>& packet,
        uint32_t size,
        uint32_t payload_source,
        uint32_t payload_length,
        s_request* request
    ) const;

    // Service execution occurs after SIF1 DMA staging/commit.
    void commit_command(const s_request& request);

    // Deliver at most one queued real service reply if SIF0 is armed.
    // Pending replies remain queued if the EE cannot accept them yet.
    bool pump();
    size_t pending_replies() const;
    uint32_t load_requests() const;

private:
    struct s_reply
    {
        uint32_t m_words[12]{};
    };

    c_disc_loader& m_loader;
    c_sifcmd_bridge& m_sifcmd;
    ee::ee_memory& m_ee_ram;
    ee::c_ee_dmac& m_dmac;
    std::deque<s_reply> m_replies;
    uint32_t m_load_requests = 0u;
};
} // namespace iop
