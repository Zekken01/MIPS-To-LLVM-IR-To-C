#pragma once

#include "iop_kernel.h"
#include "ee_sif.h"
#include "ee_memory.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace iop
{
class c_sif_rpc_bridge;

// An IOP SIFCMD endpoint, initialized either by a real R3000 guest call
// or by an explicit HLE-only kernel bootstrap. Firmware mode must never
// install or invoke this compatibility service.
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
    static constexpr uint32_t k_service_base = 0x494f0100u;
    // SIFMAN and CDVDMAN own 0x18000..0x18aff.
    static constexpr uint32_t k_extra_stub_base = 0x00018b00u;
    static constexpr uint32_t k_rpc_finish_stub = 0x00018f80u;

    struct s_command
    {
        uint32_t m_cid = 0u;
        uint32_t m_index = 0u;
        uint32_t m_value = 0u;
        bool m_request_sreg_reply = false;
        bool m_reboot_request = false;
        bool m_rpc_end = false;
        uint32_t m_rpc_client = 0u;
        uint32_t m_rpc_command = 0u;
        uint32_t m_rpc_server_data = 0u;
        uint32_t m_rpc_buffer = 0u;
        uint32_t m_rpc_cbuffer = 0u;
        std::string m_reboot_argument;
    };

    c_sifcmd_bridge(
        c_kernel& kernel, ee::c_ee_sif& sif,
        ee::ee_memory* ee_ram = nullptr
    );
    void bind_rpc_bridge(c_sif_rpc_bridge* bridge);

    // Only guest-registered RPC services can accept an EE bind/call.
    // No outgoing EE response is created here by registration alone.
    bool has_guest_rpc_server(uint32_t server_id) const;
    bool can_queue_guest_rpc(uint32_t server_id, uint32_t send_size) const;
    bool enqueue_guest_rpc(uint32_t token, uint32_t server_id,
                           uint32_t method,
                           const std::vector<uint8_t>& bytes);

    bool install();
    // Explicit no-ROM0, extracted-disc HLE service startup. Equivalent to
    // the HLE IOP kernel's initial SIFCMD + completed-bootstrap state in
    // twist84/ps2-web-recomp (SDBZ). Never invoked by mere installation,
    // guest polling, ROM0 or an ordinary IRX import.
    bool initialize_hle_bootstrap();
    // A validated reset packet is delivered separately from completed
    // IOP reboot. Only the explicit HLE reset coordinator consumes it.
    bool take_reboot_request(std::string* argument);
    void reset_for_reboot();
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
    struct s_rpc_server
    {
        uint32_t m_descriptor = 0u;
        uint32_t m_handler = 0u;
        uint32_t m_buffer = 0u;
        uint32_t m_queue = 0u;
    };

    struct s_rpc_client
    {
        uint32_t m_client = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_server_data = 0u;
        uint32_t m_ee_buffer = 0u;
        uint32_t m_packet_id = 0u;
        uint32_t m_rpc_id = 0u;
        uint32_t m_wait_thread = 0u;
        uint32_t m_expected_command = 0u;
        bool m_pending = false;
        bool m_bound = false;
    };

    struct s_rpc_request
    {
        uint32_t m_token = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_method = 0u;
        std::vector<uint8_t> m_bytes;
    };

    static bool guest_ram_span(uint32_t address, uint32_t size);
    bool handle_rpc_syscall(uint32_t ordinal);

    c_kernel& m_kernel;
    ee::c_ee_sif& m_sif;
    ee::ee_memory* m_ee_ram = nullptr;
    c_sif_rpc_bridge* m_rpc_bridge = nullptr;
    std::map<uint32_t, s_rpc_server> m_rpc_servers;
    std::map<uint32_t, s_rpc_client> m_rpc_clients;
    uint32_t m_next_iop_rpc_id = 1u;
    std::map<uint32_t, uint32_t> m_rpc_queue_threads;
    std::map<uint32_t, std::deque<s_rpc_request>> m_rpc_requests;
    std::map<uint32_t, uint32_t> m_rpc_thread_queues;
    std::map<uint32_t, uint32_t> m_rpc_thread_active;
    bool m_rpc_initialized = false;
    uint32_t m_sregs[32]{};
    uint32_t m_ee_receive_address = 0u;
    bool m_initialized = false;
    bool m_installed = false;
    bool m_hle_bootstrapped = false;
    bool m_reboot_pending = false;
    std::string m_reboot_argument;
};
} // namespace iop
