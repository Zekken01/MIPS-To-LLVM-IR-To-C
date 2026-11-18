#pragma once

#include "iop_disc_loader.h"
#include "iop_cdvd.h"
#include "iop_sifcmd_bridge.h"
#include "ee_dmac.h"
#include "ee_memory.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace iop
{
// Bounded, explicit LOADFILE and IOPHEAP RPC services connected to guest-initialized
// IOP SIFCMD. Only actual EE SIF1 bind/call packets invoke this service.
// Outgoing replies require an EE-provided receive buffer and armed SIF0.
// No IOP firmware, SMFLAG readiness, or generic RPC dispatch is synthesized.
class c_sif_rpc_bridge
{
public:
    static constexpr uint32_t k_rpc_data_destination = 0xdeadbef0u;
    static constexpr uint32_t k_loadfile_server = 0x80000006u;
    static constexpr uint32_t k_iop_heap_server = 0x80000003u;
    static constexpr uint32_t k_cdvd_init_server = 0x80000592u;
    static constexpr uint32_t k_cdvd_s_server = 0x80000593u;
    static constexpr uint32_t k_cdvd_n_server = 0x80000595u;
    static constexpr uint32_t k_cdvd_stream_server = 0x80000596u;
    static constexpr uint32_t k_cdvd_search_server = 0x80000597u;
    static constexpr uint32_t k_cdvd_ready_server = 0x8000059au;
    static constexpr uint32_t k_cdvd_ready_alt_server = 0x8000059cu;
    static constexpr uint32_t k_bind_cid = 0x80000009u;
    static constexpr uint32_t k_call_cid = 0x8000000au;
    static constexpr uint32_t k_reply_cid = 0x80000008u;
    static constexpr uint32_t k_server_xor = 0xacacacacu;

    struct s_request
    {
        uint32_t m_words[16]{};
        uint32_t m_word_count = 0u;
        uint32_t m_result_destination = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_heap_argument = 0u;
        uint32_t m_heap_file_size = 0u;
        bool m_heap_file_found = false;
        std::vector<uint8_t> m_heap_file_bytes;
        uint32_t m_cdvd_lba = 0u;
        uint32_t m_cdvd_count = 0u;
        uint32_t m_cdvd_destination = 0u;
        uint32_t m_cdvd_stream_command = 0u;
        uint32_t m_cdvd_stream_position = 0u;
        uint32_t m_cdvd_lookup_lba = 0u;
        uint32_t m_cdvd_lookup_size = 0u;
        uint32_t m_payload_source = 0u;
        bool m_cdvd_to_iop = false;
        bool m_cdvd_lookup_found = false;
        std::vector<uint8_t> m_cdvd_bytes;
        std::string m_guest_path;
    };

    // Actual committed SIFRPC operations. This diagnostic is populated
    // only after a complete SIF1 descriptor list passes validation; it
    // never infers successful IRX startup from the static disc catalog.
    struct s_rpc_observation
    {
        uint64_t m_sequence = 0u;
        uint32_t m_cid = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_method = 0u;
        uint32_t m_client = 0u;
        uint32_t m_send_size = 0u;
        uint32_t m_recv_size = 0u;
        uint32_t m_return_word = 0u;
        e_disc_load_result m_load_status =
            e_disc_load_result::invalid_guest_path;
        e_irx_load_result m_link_status = e_irx_load_result::invalid_elf;
        uint32_t m_module_id = 0u;
        uint32_t m_thread_id = 0u;
        bool m_module_started = false;
        std::string m_guest_path;
    };

    const s_rpc_observation& last_observation() const;

    c_sif_rpc_bridge(
        c_disc_loader& loader,
        c_kernel& kernel,
        c_sifcmd_bridge& sifcmd,
        ee::ee_memory& ee_ram,
        ee::c_ee_dmac& dmac,
        c_sector_source* sectors = nullptr
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

    // Invalidate outstanding RPC replies and CDVD streaming state
    // whenever the IOP kernel is actually reset.
    void reset_for_reboot();
    // Read LOADFILE's version from the actual on-disc reboot image.
    bool configure_reboot_image_version(const std::string& reboot_command);

    // The 16-byte EE INIT_CMD/opt=1 packet requests the original
    // IOP SET_SREG[0]=1 command. Reserve queue capacity during SIF1
    // validation; commit only after the entire batch is accepted.
    bool can_queue_replies(size_t count) const;
    void queue_init_sreg_reply();

    // Deliver at most one queued real service reply if SIF0 is armed.
    // Pending replies remain queued if the EE cannot accept them yet.
    bool pump();
    size_t pending_replies() const;
    uint32_t load_requests() const;

private:
    struct s_reply
    {
        uint32_t m_words[12]{};
        uint32_t m_word_count = 12u;
    };

    c_disc_loader& m_loader;
    c_kernel& m_kernel;
    c_sifcmd_bridge& m_sifcmd;
    ee::ee_memory& m_ee_ram;
    ee::c_ee_dmac& m_dmac;
    c_sector_source* m_sectors = nullptr;
    bool m_cdvd_streaming = false;
    bool m_loadfile_version_known = false;
    uint8_t m_loadfile_version[4]{};
    uint32_t m_cdvd_stream_position = 0u;
    std::deque<s_reply> m_replies;
    uint32_t m_load_requests = 0u;
    uint64_t m_total_rpc_requests = 0u;
    s_rpc_observation m_last_observation{};
    bool m_reply_in_flight = false;
};
} // namespace iop
