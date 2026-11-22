// SIO2 memory card protocol adapted from ps2-web-recomp/SDBZ, MIT license.
// See src/runtime/ps2_webgpu/gs/LICENSE.upstream.
#include "iop_memory_card.h"

#include <algorithm>
#include <fstream>

namespace iop
{
bool c_memory_card::mount(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (mounted() || !input ||
        input.tellg() != static_cast<std::streamoff>(k_image_bytes))
    {
        return false;
    }
    std::vector<uint8_t> data(k_image_bytes);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), data.size()))
    {
        return false;
    }
    m_path = path;
    m_data.swap(data);
    reset_protocol();
    return true;
}

void c_memory_card::reset_protocol()
{
    m_address = 0u;
    m_terminator = 0x55u;
    m_auth_reset_pending = false;
}

bool c_memory_card::complete_auth_reset()
{
    const bool pending = m_auth_reset_pending;
    m_auth_reset_pending = false;
    return pending;
}

bool c_memory_card::exchange(const uint8_t* input, size_t size,
                             std::vector<uint8_t>& reply)
{
    if (!mounted() || size < 2u || input[0] != 0x81u)
    {
        return false;
    }

    const auto put = [&reply](size_t index, uint8_t value)
    {
        if (index < reply.size())
        {
            reply[index] = value;
        }
    };
    const auto acknowledge = [&]()
    {
        if (reply.size() >= 4u)
        {
            std::fill(reply.begin() + 2, reply.end() - 2, 0u);
            reply[reply.size() - 2] = 0x2bu;
            reply.back() = m_terminator;
        }
    };

    switch (input[1])
    {
    case 0x21u: // Erase address
    case 0x22u: // Write address
    case 0x23u: // Read address
    {
        if (size < 6u)
        {
            return false;
        }
        const uint32_t page = static_cast<uint32_t>(input[2]) |
            (static_cast<uint32_t>(input[3]) << 8u) |
            (static_cast<uint32_t>(input[4]) << 16u) |
            (static_cast<uint32_t>(input[5]) << 24u);
        if (page >= k_pages)
        {
            return false;
        }
        m_address = page * k_page_bytes;
        acknowledge();
        break;
    }
    case 0x26u: // Geometry
    {
        constexpr uint8_t spec[]{0u, 2u, k_block_pages, 0u, 0u, 0x40u, 0u, 0u};
        put(2u, 0x2bu);
        uint8_t checksum = 0u;
        for (size_t index = 0u; index < sizeof(spec); ++index)
        {
            put(3u + index, spec[index]);
            checksum ^= spec[index];
        }
        put(11u, checksum);
        put(12u, m_terminator);
        break;
    }
    case 0x27u: // Set terminator
        if (size < 3u)
        {
            return false;
        }
        m_terminator = input[2];
        acknowledge();
        break;
    case 0x28u: // Get terminator
        put(2u, 0x2bu);
        put(3u, m_terminator);
        put(4u, 0x55u);
        break;
    case 0x42u: // Write chunk
    {
        if (size < 4u)
        {
            return false;
        }
        const size_t count = input[2];
        if (count > size - 4u || count > m_data.size() - m_address)
        {
            return false;
        }
        uint8_t checksum = 0u;
        for (size_t index = 0u; index < count; ++index)
        {
            checksum ^= input[3u + index];
        }
        if (checksum != input[3u + count])
        {
            return false;
        }
        std::copy_n(input + 3u, count, m_data.begin() + m_address);
        m_address += count;
        m_dirty = m_dirty || count != 0u;
        acknowledge();
        break;
    }
    case 0x43u: // Read chunk
    {
        if (size < 3u)
        {
            return false;
        }
        const size_t count = input[2];
        if (count > m_data.size() - m_address || reply.size() < count + 6u)
        {
            return false;
        }
        put(2u, 0u);
        put(3u, 0x2bu);
        uint8_t checksum = 0u;
        for (size_t index = 0u; index < count; ++index)
        {
            const uint8_t value = m_data[m_address++];
            put(4u + index, value);
            checksum ^= value;
        }
        put(4u + count, checksum);
        put(5u + count, m_terminator);
        break;
    }
    case 0x82u: // Erase block
    {
        if (m_address >= m_data.size())
        {
            return false;
        }
        constexpr size_t k_block_bytes = k_page_bytes * k_block_pages;
        const size_t block = m_address / k_block_bytes * k_block_bytes;
        std::fill_n(m_data.begin() + block, k_block_bytes, 0xffu);
        m_dirty = true;
        acknowledge();
        break;
    }
    case 0xf3u: // Reset authentication state for the virtual card.
        if (size < 5u || reply.size() < 5u)
        {
            return false;
        }
        m_auth_reset_pending = true;
        acknowledge();
        break;
    case 0x11u: // Probe
    case 0x12u: // End erase
    case 0x81u: // End read/write
    case 0xbfu: // PS2 card initialization command
        acknowledge();
        break;
    default:
        return false;
    }
    ++m_completed_exchanges;
    return true;
}

bool c_memory_card::save()
{
    if (!m_dirty)
    {
        return true;
    }
    // Write beside the image so rename can replace it atomically.
    // Refuse to reuse an existing working directory.
    auto work = m_path;
    work += ".pending";
    std::error_code error;
    if (!std::filesystem::create_directory(work, error))
    {
        return false;
    }
    const auto pending = work / "card";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(m_data.data()), m_data.size());
    output.close();
    bool saved = false;
    if (output)
    {
        std::filesystem::rename(pending, m_path, error);
        saved = !error;
    }
    std::filesystem::remove(pending, error);
    std::filesystem::remove(work, error);
    if (saved)
    {
        m_dirty = false;
    }
    return saved;
}
}
