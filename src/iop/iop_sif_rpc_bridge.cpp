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
    c_disc_loader& loader, c_sifcmd_bridge& sifcmd,
    ee::ee_memory& ee_ram, ee::c_ee_dmac& dmac
)
    : m_loader(loader), m_sifcmd(sifcmd),
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
        (size != 36u && size != 56u) ||
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
    if (cid == k_bind_cid && size == 36u)
    {
        if (packet[8] != k_loadfile_server)
        {
            return false;
        }
        *request = std::move(candidate);
        return true;
    }

    if (cid != k_call_cid || size != 56u ||
        packet[13] != (k_loadfile_server ^ k_server_xor) ||
        packet[8] != 0u || // method 0 is sceSifLoadModule
        packet[9] < 260u || packet[9] > 4096u ||
        packet[11] < 8u || packet[11] > 4096u ||
        packet[12] > 1u ||
        payload_length < packet[9])
    {
        return false;
    }

    uint32_t result = 0u;
    if (!normalize_ee_ram(packet[10], &result) ||
        !m_ee_ram.contains(result, 8u) ||
        !m_ee_ram.contains(payload_source, packet[9]) ||
        !read_guest_path(
            m_ee_ram, payload_source, packet[9],
            &candidate.m_guest_path))
    {
        return false;
    }
    candidate.m_result_destination = result;
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
        response.m_words[9] = k_loadfile_server ^ k_server_xor;
        response.m_words[10] = k_rpc_data_destination;
        response.m_words[11] = 0xdeadcafeu;
    }
    else if (request.m_words[2] == k_call_cid)
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
    m_replies.push_back(response);
    (void)pump();
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
    ee::s_ee_u128 chcr{};
    if (destination == 0u ||
        (destination & 0x0fu) != 0u ||
        !m_ee_ram.contains(destination, 48u) ||
        !ee::c_ee_dmac::mmio_read(
            &m_dmac, ee::c_ee_dmac::k_sif0_base, 4u, &chcr) ||
        (chcr.m_lo & ee::c_ee_dmac::k_chcr_str) == 0u)
    {
        return false;
    }

    const s_reply& reply = m_replies.front();
    for (uint32_t index = 0u; index < 12u; ++index)
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
