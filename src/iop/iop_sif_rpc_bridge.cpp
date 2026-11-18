#include "iop_sif_rpc_bridge.h"

#include <algorithm>
#include <utility>

namespace iop
{
namespace
{
bool normalize_ee_ram(uint32_t pointer, uint32_t* physical)
{
    if (physical == nullptr)
    {
        return false;
    }
    if (pointer < 0x02000000u)
    {
        *physical = pointer;
        return true;
    }
    if ((pointer >= 0x80000000u && pointer < 0xc0000000u) ||
        ((pointer >> 28u) == 2u || (pointer >> 28u) == 3u))
    {
        *physical = pointer & 0x1fffffffu;
        return *physical < 0x02000000u;
    }
    return false;
}

bool read_guest_path(
    ee::ee_memory& memory, uint32_t buffer,
    uint32_t size, std::string* output
)
{
    // LOADFILE method 0: word arglen, word unused, 252-byte path,
    // followed by up to 252 bytes of IOP module arguments. Module
    // arguments are not yet supported: reject rather than discard.
    if (output == nullptr || size < 260u ||
        !memory.contains(buffer, size) ||
        memory.read32(buffer) != 0u)
    {
        return false;
    }
    std::string path;
    for (uint32_t index = 0u; index < 252u; ++index)
    {
        const uint8_t ch = memory.read8(buffer + 8u + index);
        if (ch == 0u)
        {
            if (path.empty())
            {
                return false;
            }
            *output = std::move(path);
            return true;
        }
        if (ch < 0x20u || ch > 0x7eu)
        {
            return false;
        }
        path.push_back(static_cast<char>(ch));
    }
    return false;
}
} // namespace

c_sif_rpc_bridge::c_sif_rpc_bridge(
    c_disc_loader& loader, c_kernel& kernel,
    c_sifcmd_bridge& sifcmd, ee::ee_memory& ee_ram,
    ee::c_ee_dmac& dmac
)
    : m_loader(loader), m_kernel(kernel), m_sifcmd(sifcmd),
      m_ee_ram(ee_ram), m_dmac(dmac)
{
}

bool c_sif_rpc_bridge::prepare_command(
    const std::vector<uint32_t>& packet,
    uint32_t size,
    uint32_t payload_source,
    uint32_t payload_length,
    s_request* request
) const
{
    if (!m_sifcmd.initialized() ||
        request == nullptr ||
        m_replies.size() >= 64u ||
        packet.size() < size / 4u ||
        (size != 36u && size != 56u && size != 64u) ||
        (packet[0] & 0xffu) != size ||
        packet[1] != 0u || packet[3] != 0u ||
        packet[4] == 0u || packet[5] == 0u ||
        packet[6] == 0u || packet[7] == 0u)
    {
        return false;
    }

    s_request candidate{};
    candidate.m_word_count = size / 4u;
    for (uint32_t index = 0u; index < candidate.m_word_count; ++index)
    {
        candidate.m_words[index] = packet[index];
    }
    const uint32_t cid = packet[2];
    // Standard EE sifrpc also emits a 64-byte bind packet. The
    // first 36 bytes carry the same header and server identity as
    // the shorter form (as decoded by ps2-web-recomp's bind()).
    // The remaining bytes are reserved/padding, not a second RPC.
    if (cid == k_bind_cid && (size == 36u || size == 64u))
    {
        if (packet[8] != k_loadfile_server &&
            packet[8] != k_iop_heap_server)
        {
            return false;
        }
        candidate.m_server_id = packet[8];
        *request = std::move(candidate);
        return true;
    }

    if (cid != k_call_cid ||
        (size != 56u && size != 64u) ||
        packet[9] > 4096u ||
        packet[11] > 4096u ||
        packet[12] > 1u ||
        payload_length < packet[9])
    {
        return false;
    }

    const uint32_t server_id = packet[13] ^ k_server_xor;
    const uint32_t method = packet[8];
    uint32_t result = 0u;
    if (!normalize_ee_ram(packet[10], &result))
    {
        return false;
    }
    candidate.m_server_id = server_id;
    candidate.m_result_destination = result;
    if (server_id == k_loadfile_server)
    {
        if (method != 0u || packet[9] < 260u ||
            packet[11] < 8u ||
            !m_ee_ram.contains(result, 8u) ||
            !m_ee_ram.contains(payload_source, packet[9]) ||
            !read_guest_path(
                m_ee_ram, payload_source, packet[9],
                &candidate.m_guest_path))
        {
            return false;
        }
    }
    else if (server_id == k_iop_heap_server)
    {
        // IOPHEAP methods match ps2-web-recomp/SDBZ:
        // 1 alloc, 2 free, 4 system alloc, 5 size,
        // 6 maximum free, 7 total free.
        // Method 3 loads a file into IOP RAM and is not yet modeled.
        const bool has_argument =
            method == 1u || method == 2u || method == 4u;
        if ((method != 1u && method != 2u && method != 4u &&
             method != 5u && method != 6u && method != 7u) ||
            (has_argument && packet[9] < (method == 4u ? 12u : 4u)) ||
            packet[11] < 4u ||
            !m_ee_ram.contains(result, 4u) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return false;
        }
        if (has_argument)
        {
            candidate.m_heap_argument =
                m_ee_ram.read32(payload_source);
        }
    }
    else
    {
        return false;
    }
    *request = std::move(candidate);
    return true;
}

void c_sif_rpc_bridge::commit_command(const s_request& request)
{
    s_reply response{};
    // SIFCMD Rend: 48-byte header, echoed guest transaction identity,
    // and the command serviced. No reply is queued before real request
    // validation and LOADFILE backend execution.
    response.m_words[0] = 48u;
    response.m_words[1] = request.m_words[1];
    response.m_words[2] = k_reply_cid;
    response.m_words[3] = 0u;
    response.m_words[4] = request.m_words[4];
    response.m_words[5] = request.m_words[5];
    response.m_words[6] = request.m_words[6];
    response.m_words[7] = request.m_words[7];
    response.m_words[8] = request.m_words[2];

    if (request.m_words[2] == k_bind_cid)
    {
        response.m_words[9] = request.m_server_id ^ k_server_xor;
        response.m_words[10] = k_rpc_data_destination;
        response.m_words[11] = 0xdeadcafeu;
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_loadfile_server)
    {
        ++m_load_requests;
        const s_disc_load_result loaded = m_loader.load_requested(
            request.m_guest_path, true
        );
        // A completed HLE LOADFILE call returns its actual module
        // identifier or an error, never success for a missing module.
        // The complete IOP module's guest thread is then scheduled.
        const uint32_t result =
            loaded.m_result == e_disc_load_result::success
                ? loaded.m_module_id
                : static_cast<uint32_t>(-1);
        m_ee_ram.write32(request.m_result_destination, result);
        m_ee_ram.write32(
            request.m_result_destination + 4u,
            loaded.m_result == e_disc_load_result::success
                ? 0u : static_cast<uint32_t>(-1)
        );
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_iop_heap_server)
    {
        uint32_t result = 0u;
        switch (request.m_words[8])
        {
        case 1u:
        case 4u:
            result = m_kernel.allocate_heap(request.m_heap_argument);
            break;
        case 2u:
            result = m_kernel.free_heap(request.m_heap_argument)
                ? 0u : static_cast<uint32_t>(-1);
            break;
        case 5u:
            result = c_cpu::k_ram_size;
            break;
        case 6u:
            result = m_kernel.heap_max_free();
            break;
        case 7u:
            result = m_kernel.heap_total_free();
            break;
        default:
            // Unreachable: prepare_command refuses unimplemented methods.
            result = static_cast<uint32_t>(-1);
            break;
        }
        m_ee_ram.write32(request.m_result_destination, result);
    }
    m_replies.push_back(response);
    (void)pump();
}

bool c_sif_rpc_bridge::can_queue_replies(size_t count) const
{
    return count <= 64u && m_replies.size() <= 64u - count;
}

void c_sif_rpc_bridge::queue_init_sreg_reply()
{
    // This packet exists only in response to a validated EE INIT_CMD
    // opt=1 request. The guest EE handler consumes it via armed SIF0;
    // do not write its SREG or mark any IOP module as started here.
    s_reply reply{};
    reply.m_word_count = 6u;
    reply.m_words[0] = 24u;
    reply.m_words[2] = 0x80000001u; // SET_SREG
    reply.m_words[4] = 0u;          // SREG index
    reply.m_words[5] = 1u;          // initialized
    m_replies.push_back(reply);
}

bool c_sif_rpc_bridge::pump()
{
    if (m_reply_in_flight)
    {
        // The prior SIF0 interrupt must have been acknowledged before
        // overwriting the EE command receiver with another reply.
        if ((m_dmac.status() & (1u << 5u)) != 0u)
        {
            return false;
        }
        m_reply_in_flight = false;
    }
    if (m_replies.empty() || !m_sifcmd.initialized())
    {
        return false;
    }

    const uint32_t destination = m_sifcmd.ee_receive_address();
    const s_reply& reply = m_replies.front();
    ee::s_ee_u128 chcr{};
    if (destination == 0u ||
        (destination & 0x0fu) != 0u ||
        !m_ee_ram.contains(destination, reply.m_word_count * 4u) ||
        !ee::c_ee_dmac::mmio_read(
            &m_dmac, ee::c_ee_dmac::k_sif0_base, 4u, &chcr) ||
        (chcr.m_lo & ee::c_ee_dmac::k_chcr_str) == 0u)
    {
        return false;
    }

    for (uint32_t index = 0u; index < reply.m_word_count; ++index)
    {
        m_ee_ram.write32(destination + index * 4u,
                         reply.m_words[index]);
    }
    if (!m_dmac.raise(5u))
    {
        return false;
    }
    m_replies.pop_front();
    m_reply_in_flight = true;
    return true;
}

size_t c_sif_rpc_bridge::pending_replies() const
{
    return m_replies.size();
}

uint32_t c_sif_rpc_bridge::load_requests() const
{
    return m_load_requests;
}
} // namespace iop
