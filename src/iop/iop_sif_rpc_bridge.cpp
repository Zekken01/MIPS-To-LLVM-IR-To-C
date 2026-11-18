#include "iop_sif_rpc_bridge.h"
#include "iop_folder_sector_source.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <sstream>
#include <limits>
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
    ee::c_ee_dmac& dmac, c_sector_source* sectors
)
    : m_loader(loader), m_kernel(kernel), m_sifcmd(sifcmd),
      m_ee_ram(ee_ram), m_dmac(dmac), m_sectors(sectors)
{
}

bool c_sif_rpc_bridge::configure_reboot_image_version(
    const std::string& reboot_command
)
{
    m_loadfile_version_known = false;
    std::fill(std::begin(m_loadfile_version),
              std::end(m_loadfile_version), uint8_t{0});

    const auto* folder = dynamic_cast<c_folder_sector_source*>(m_sectors);
    if (folder == nullptr)
    {
        return false;
    }
    std::istringstream arguments(reboot_command);
    std::string loader;
    std::string path;
    if (!(arguments >> loader))
    {
        return false;
    }
    while (arguments >> path)
    {
        if (path != "-v")
        {
            break;
        }
    }
    s_folder_file_info image{};
    constexpr uint32_t k_max_image = 16u * 1024u * 1024u;
    if (path.empty() || !folder->find_file(path, &image) ||
        image.m_size < 16u || image.m_size > k_max_image)
    {
        return false;
    }
    const uint32_t count =
        (image.m_size + c_iso_sector_source::k_sector_size - 1u) /
        c_iso_sector_source::k_sector_size;
    std::vector<uint8_t> bytes;
    if (!m_sectors->read_sectors(image.m_lba, count, bytes) ||
        bytes.size() < image.m_size)
    {
        return false;
    }
    static constexpr char k_marker[] = "PsIIloadfile";
    const auto end = bytes.begin() + image.m_size;
    const auto marker = std::search(
        bytes.begin(), end, std::begin(k_marker), std::end(k_marker) - 1);
    if (marker == end ||
        static_cast<size_t>(end - marker) < sizeof(k_marker) - 1u + 4u)
    {
        return false;
    }
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const uint8_t digit = marker[sizeof(k_marker) - 1u + index];
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        m_loadfile_version[index] = digit;
    }
    m_loadfile_version_known = true;
    return true;
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
        // sifrpc uses DEADBEF0 as its IOP-side data destination.
        // Earlier synthetic packets also used zero. Neither header
        // form licenses an arbitrary IOP destination.
        (packet[1] != 0u &&
         packet[1] != k_rpc_data_destination) ||
        packet[3] != 0u ||
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
        // CDVDFSV's CdInit server is a built-in HLE service,
        // as in ps2-web-recomp/SDBZ. This only binds the client;
        // unsupported CDVD RPC methods must still fail closed.
        if (packet[8] != k_loadfile_server &&
            packet[8] != k_iop_heap_server &&
            packet[8] != k_cdvd_init_server &&
            !(m_sectors != nullptr && m_sectors->sector_count() != 0u &&
              (packet[8] == k_cdvd_s_server ||
               packet[8] == k_cdvd_n_server ||
               packet[8] == k_cdvd_stream_server ||
               packet[8] == k_cdvd_search_server ||
               packet[8] == k_cdvd_ready_server ||
               packet[8] == k_cdvd_ready_alt_server)))
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
        if (method == 0xffu)
        {
            // The retail post-reboot request carries no payload.
            // Only verified IOPRP metadata is returned.
            if (!m_loadfile_version_known || packet[9] != 0u ||
                packet[11] < 4u ||
                !m_ee_ram.contains(result, packet[11]))
            {
                return false;
            }
        }
        else if (method != 0u || packet[9] < 260u ||
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
    else if (server_id == k_cdvd_init_server)
    {
        // Retail SDBZ calls CdInit method 0. The observed word[4] = 5
        // is the RPC record ID, not the method (which is word[8]).
        // The reference HLE accepts a bounded receive size. Only a
        // buffer of at least sixteen bytes receives result[3] = 0xff.
        // Other methods remain unsupported until independently verified.
        if (method != 0u ||
            !m_ee_ram.contains(result, packet[11]) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return false;
        }
    }
    else if (server_id == k_iop_heap_server)
    {
        // IOPHEAP 1/2/4/5/6/7 share the real kernel allocator.
        // Method 3 transfers actual mounted file bytes to a live
        // IOPHEAP buffer, rather than writing kernel or IRX memory.
        const bool has_argument =
            method == 1u || method == 2u || method == 3u ||
            method == 4u;
        if ((method < 1u || method > 7u) ||
            (has_argument && packet[9] <
                (method == 3u ? 6u : (method == 4u ? 12u : 4u))) ||
            packet[11] < 4u ||
            !m_ee_ram.contains(result, 4u) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return false;
        }
        if (has_argument)
        {
            candidate.m_heap_argument = m_ee_ram.read32(payload_source);
        }
        if (method == 3u)
        {
            const auto* folder =
                dynamic_cast<c_folder_sector_source*>(m_sectors);
            if (folder == nullptr)
            {
                return false;
            }
            const uint32_t region =
                candidate.m_heap_argument & 0xe0000000u;
            if (region != 0u && region != 0x80000000u &&
                region != 0xa0000000u)
            {
                return false;
            }
            candidate.m_heap_argument &= 0x1fffffffu;
            std::string path;
            bool terminated = false;
            for (uint32_t index = 4u; index < packet[9]; ++index)
            {
                const uint8_t ch =
                    m_ee_ram.read8(payload_source + index);
                if (ch == 0u)
                {
                    terminated = true;
                    break;
                }
                if (ch < 0x21u || ch > 0x7eu ||
                    path.size() >= 255u)
                {
                    return false;
                }
                path.push_back(static_cast<char>(ch));
            }
            if (!terminated || path.size() < 8u ||
                path.find("..") != std::string::npos)
            {
                return false;
            }
            s_folder_file_info file{};
            if (!folder->find_file(path, &file))
            {
                // Legitimate filesystem miss: reply -2, not success.
                candidate.m_heap_file_found = false;
            }
            else
            {
                const uint32_t count =
                    (file.m_size + c_iso_sector_source::k_sector_size -
                     1u) / c_iso_sector_source::k_sector_size;
                if (file.m_size == 0u ||
                    !m_kernel.owns_heap_span(
                        candidate.m_heap_argument, file.m_size) ||
                    static_cast<uint64_t>(file.m_lba) + count >
                        m_sectors->sector_count() ||
                    !m_sectors->read_sectors(
                        file.m_lba, count, candidate.m_heap_file_bytes) ||
                    candidate.m_heap_file_bytes.size() < file.m_size)
                {
                    return false;
                }
                candidate.m_heap_file_size = file.m_size;
                candidate.m_heap_file_bytes.resize(file.m_size);
                candidate.m_heap_file_found = true;
            }
        }
    }
    else if (server_id == k_cdvd_s_server ||
             server_id == k_cdvd_n_server ||
             server_id == k_cdvd_stream_server ||
             server_id == k_cdvd_search_server ||
             server_id == k_cdvd_ready_server ||
             server_id == k_cdvd_ready_alt_server)
    {
        // These HLE services exist only when actual disc media is mounted.
        // Every request is fully validated before SIF1 changes guest RAM.
        if (m_sectors == nullptr || m_sectors->sector_count() == 0u ||
            !m_ee_ram.contains(result, packet[11]) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return false;
        }
        candidate.m_payload_source = payload_source;
        const uint32_t reply_size = packet[11];
        if (server_id == k_cdvd_s_server)
        {
            uint32_t needed = 4u;
            switch (method)
            {
            case 0x01u: needed = 12u; break;
            case 0x05u: needed = 8u; break;
            case 0x27u:
                // This single-layer directory model cannot report a
                // fabricated DVD second-layer boundary.
                if (dynamic_cast<c_folder_sector_source*>(m_sectors) ==
                    nullptr)
                {
                    return false;
                }
                needed = 12u;
                break;
            case 0x03u:
            case 0x04u:
            case 0x0cu:
            case 0x16u:
            case 0x22u:
                break;
            default: return false;
            }
            if (reply_size < needed)
            {
                return false;
            }
        }
        else if (server_id == k_cdvd_n_server)
        {
            uint32_t needed = 0u;
            bool read_media = false;
            switch (method)
            {
            case 0x01u:
            case 0x0du:
                if (packet[9] < 12u)
                {
                    return false;
                }
                candidate.m_cdvd_lba = m_ee_ram.read32(payload_source);
                candidate.m_cdvd_count =
                    m_ee_ram.read32(payload_source + 4u);
                candidate.m_cdvd_destination =
                    m_ee_ram.read32(payload_source + 8u);
                candidate.m_cdvd_to_iop = method == 0x0du;
                needed = 4u;
                read_media = true;
                break;
            case 0x09u:
                if (packet[9] < 16u)
                {
                    return false;
                }
                candidate.m_cdvd_lba = m_ee_ram.read32(payload_source);
                candidate.m_cdvd_count =
                    m_ee_ram.read32(payload_source + 4u);
                candidate.m_cdvd_destination =
                    m_ee_ram.read32(payload_source + 8u);
                candidate.m_cdvd_stream_command =
                    m_ee_ram.read32(payload_source + 12u);
                if (candidate.m_cdvd_stream_command == 2u)
                {
                    candidate.m_cdvd_lba = m_cdvd_stream_position;
                    read_media = true;
                }
                else if (candidate.m_cdvd_stream_command != 1u &&
                         candidate.m_cdvd_stream_command != 3u &&
                         candidate.m_cdvd_stream_command != 4u &&
                         candidate.m_cdvd_stream_command != 5u &&
                         candidate.m_cdvd_stream_command != 6u &&
                         candidate.m_cdvd_stream_command != 9u)
                {
                    return false;
                }
                needed = 4u;
                break;
            case 0x04u:
            case 0x0eu:
                needed = 4u;
                break;
            case 0x05u:
            case 0x08u:
                break;
            default: return false;
            }
            if (reply_size < needed)
            {
                return false;
            }
            if (read_media)
            {
                const uint64_t bytes =
                    static_cast<uint64_t>(candidate.m_cdvd_count) *
                    c_iso_sector_source::k_sector_size;
                if (candidate.m_cdvd_count == 0u ||
                    bytes > c_cpu::k_ram_size ||
                    static_cast<uint64_t>(candidate.m_cdvd_lba) +
                        candidate.m_cdvd_count > m_sectors->sector_count())
                {
                    return false;
                }
                uint32_t destination = candidate.m_cdvd_destination;
                if (candidate.m_cdvd_to_iop)
                {
                    const uint32_t region = destination & 0xe0000000u;
                    if ((region != 0u && region != 0x80000000u &&
                         region != 0xa0000000u) ||
                        (destination & 0x1fffffffu) >
                            c_cpu::k_ram_size - bytes)
                    {
                        return false;
                    }
                }
                else if (!normalize_ee_ram(destination, &destination) ||
                         !m_ee_ram.contains(
                             destination, static_cast<size_t>(bytes)))
                {
                    return false;
                }
                candidate.m_cdvd_destination = destination;
                if (!m_sectors->read_sectors(
                        candidate.m_cdvd_lba, candidate.m_cdvd_count,
                        candidate.m_cdvd_bytes) ||
                    candidate.m_cdvd_bytes.size() != bytes)
                {
                    return false;
                }
            }
        }
        else if (server_id == k_cdvd_search_server)
        {
            const auto* folder =
                dynamic_cast<c_folder_sector_source*>(m_sectors);
            const uint32_t offset =
                packet[9] == 0x124u ? 0x20u : 0x24u;
            if (folder == nullptr || packet[9] <= offset ||
                reply_size < 4u)
            {
                return false;
            }
            std::string path;
            bool terminated = false;
            for (uint32_t index = offset; index < packet[9]; ++index)
            {
                const uint8_t ch = m_ee_ram.read8(
                    payload_source + index);
                if (ch == 0u)
                {
                    terminated = true;
                    break;
                }
                if (ch < 0x20u || ch > 0x7eu ||
                    path.size() >= 255u)
                {
                    return false;
                }
                path.push_back(static_cast<char>(ch));
            }
            if (!terminated || path.empty())
            {
                return false;
            }
            const std::string qualified =
                path.size() >= 7u &&
                (path.substr(0u, 7u) == "cdrom0:" ||
                 path.substr(0u, 7u) == "CDROM0:")
                    ? path : "cdrom0:" + path;
            s_folder_file_info file{};
            candidate.m_cdvd_lookup_found =
                folder->find_file(qualified, &file);
            if (candidate.m_cdvd_lookup_found)
            {
                candidate.m_cdvd_lookup_lba = file.m_lba;
                candidate.m_cdvd_lookup_size = file.m_size;
            }
        }
        else if (server_id == k_cdvd_stream_server)
        {
            // Reference 0x80000596 method 1 holds the RPC open;
            // scheduling that operation is not available yet.
            if (method == 1u)
            {
                return false;
            }
        }
        else if (reply_size < 4u)
        {
            return false;
        }
    }
    else
    {
        return false;
    }
    *request = std::move(candidate);
    return true;
}

const c_sif_rpc_bridge::s_rpc_observation&
c_sif_rpc_bridge::last_observation() const
{
    return m_last_observation;
}

void c_sif_rpc_bridge::commit_command(const s_request& request)
{
    // Track the actual committed packet; a static ELF path is not
    // evidence that the IOP loaded or executed a module.
    s_rpc_observation observed{};
    observed.m_sequence = ++m_total_rpc_requests;
    observed.m_cid = request.m_words[2];
    observed.m_server_id = request.m_server_id;
    observed.m_client = request.m_words[7];
    if (observed.m_cid == k_call_cid)
    {
        observed.m_method = request.m_words[8];
        observed.m_send_size = request.m_words[9];
        observed.m_recv_size = request.m_words[11];
    }
    observed.m_guest_path = request.m_guest_path;
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
        if (request.m_words[8] == 0xffu)
        {
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                m_ee_ram.write8(request.m_result_destination + index,
                                m_loadfile_version[index]);
            }
        }
        else
        {
            ++m_load_requests;
            const s_disc_load_result loaded = m_loader.load_requested(
                request.m_guest_path, true
            );
            observed.m_load_status = loaded.m_result;
            observed.m_link_status = loaded.m_link_status;
            observed.m_module_id = loaded.m_module_id;
            observed.m_thread_id = loaded.m_thread_id;
            observed.m_module_started =
                loaded.m_result == e_disc_load_result::success &&
                loaded.m_thread_id != 0u;
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
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_init_server)
    {
        // CdInit always completes, but only updates result[3] if
        // the caller provided at least sixteen receive bytes.
        // This does not mark disc hardware ready or start IRX code.
        if (request.m_words[11] >= 16u)
        {
            m_ee_ram.write32(request.m_result_destination + 12u, 0xffu);
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_s_server)
    {
        const uint32_t destination = request.m_result_destination;
        const uint32_t method = request.m_words[8];
        if (method == 0x01u)
        {
            const std::time_t current = std::time(nullptr);
            const std::tm* clock = std::gmtime(&current);
            if (clock != nullptr)
            {
                const auto bcd = [](int number) -> uint8_t
                {
                    return static_cast<uint8_t>(
                        ((number / 10) << 4) | (number % 10));
                };
                m_ee_ram.write32(destination, 1u);
                const uint8_t bytes[8] = {
                    0u, bcd(clock->tm_sec), bcd(clock->tm_min),
                    bcd((clock->tm_hour + 9) % 24), 0u,
                    bcd(clock->tm_mday), bcd(clock->tm_mon + 1),
                    bcd(clock->tm_year % 100)
                };
                for (uint32_t index = 0u; index < 8u; ++index)
                {
                    m_ee_ram.write8(destination + 4u + index, bytes[index]);
                }
            }
        }
        else
        {
            uint32_t value = 1u;
            switch (method)
            {
            case 0x03u: value = 0x14u; break;
            case 0x04u: value = 0u; break;
            case 0x0cu: value = m_cdvd_streaming ? 0x12u : 0x0au; break;
            default: break;
            }
            m_ee_ram.write32(destination, value);
            if (method == 0x05u || method == 0x27u)
            {
                m_ee_ram.write32(destination + 4u, 0u);
            }
            if (method == 0x27u)
            {
                m_ee_ram.write32(destination + 8u, 0u);
            }
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_n_server)
    {
        const uint32_t method = request.m_words[8];
        if (!request.m_cdvd_bytes.empty())
        {
            if (request.m_cdvd_to_iop)
            {
                (void)m_kernel.cpu().dma_write(
                    request.m_cdvd_destination,
                    request.m_cdvd_bytes.data(),
                    static_cast<uint32_t>(request.m_cdvd_bytes.size()));
            }
            else
            {
                for (size_t index = 0u;
                     index < request.m_cdvd_bytes.size(); ++index)
                {
                    m_ee_ram.write8(
                        request.m_cdvd_destination +
                            static_cast<uint32_t>(index),
                        request.m_cdvd_bytes[index]);
                }
            }
        }
        switch (method)
        {
        case 0x01u:
        case 0x0du:
            m_ee_ram.write32(request.m_result_destination, 0u);
            break;
        case 0x04u:
            m_ee_ram.write32(request.m_result_destination, 1u);
            break;
        case 0x09u:
            switch (request.m_cdvd_stream_command)
            {
            case 1u:
                m_cdvd_stream_position = request.m_cdvd_lba;
                m_cdvd_streaming = true;
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 2u:
                m_cdvd_stream_position += request.m_cdvd_count;
                m_ee_ram.write32(
                    request.m_result_destination, request.m_cdvd_count);
                break;
            case 3u:
                m_cdvd_streaming = false;
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 4u:
            case 9u:
                m_cdvd_stream_position = request.m_cdvd_lba;
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 5u:
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 6u:
                m_ee_ram.write32(
                    request.m_result_destination, request.m_cdvd_lba);
                break;
            default: break;
            }
            break;
        case 0x0eu:
            m_ee_ram.write32(request.m_result_destination, 2u);
            break;
        default:
            // 0x05 has no output; 0x08 completes its own RPC.
            break;
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_search_server)
    {
        if (request.m_cdvd_lookup_found)
        {
            m_ee_ram.write32(
                request.m_payload_source, request.m_cdvd_lookup_lba);
            m_ee_ram.write32(
                request.m_payload_source + 4u, request.m_cdvd_lookup_size);
        }
        m_ee_ram.write32(
            request.m_result_destination,
            request.m_cdvd_lookup_found ? 1u : 0u);
    }
    else if (request.m_words[2] == k_call_cid &&
             (request.m_server_id == k_cdvd_ready_server ||
              request.m_server_id == k_cdvd_ready_alt_server))
    {
        m_ee_ram.write32(request.m_result_destination, 2u);
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_iop_heap_server)
    {
        uint32_t result = 0u;
        switch (request.m_words[8])
        {
        case 3u:
            if (request.m_heap_file_found)
            {
                const bool copied = m_kernel.cpu().dma_write(
                    request.m_heap_argument,
                    request.m_heap_file_bytes.data(),
                    request.m_heap_file_size);
                result = copied ? 0u : static_cast<uint32_t>(-1);
            }
            else
            {
                result = static_cast<uint32_t>(-2);
            }
            break;
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
    if (observed.m_cid == k_call_cid &&
        observed.m_recv_size >= 4u &&
        m_ee_ram.contains(request.m_result_destination, 4u))
    {
        observed.m_return_word =
            m_ee_ram.read32(request.m_result_destination);
    }
    m_last_observation = std::move(observed);
    m_replies.push_back(response);
    (void)pump();
}

void c_sif_rpc_bridge::reset_for_reboot()
{
    m_loadfile_version_known = false;
    std::fill(std::begin(m_loadfile_version),
              std::end(m_loadfile_version), uint8_t{0});
    m_cdvd_streaming = false;
    m_cdvd_stream_position = 0u;
    m_replies.clear();
    m_load_requests = 0u;
    m_last_observation = {};
    m_reply_in_flight = false;
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
