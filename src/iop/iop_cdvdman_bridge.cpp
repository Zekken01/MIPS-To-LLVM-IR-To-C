#include "iop_cdvdman_bridge.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace iop
{
namespace
{
bool create_stub(c_cpu& cpu, uint32_t pc, uint32_t service)
{
    // lui v0, upper; ori v0, lower; syscall; jr ra; nop.
    const uint32_t words[5] =
    {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0x00000000u
    };
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        if (!cpu.write32(pc + i * 4u, words[i]))
        {
            return false;
        }
    }
    return true;
}

void put32(uint8_t* bytes, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[i] = static_cast<uint8_t>(value >> (8u * i));
    }
}
} // namespace

c_cdvdman_bridge::c_cdvdman_bridge(
    c_kernel& kernel, const c_folder_sector_source& source
) : m_kernel(kernel), m_source(source)
{
}

bool c_cdvdman_bridge::install()
{
    return m_source.valid() &&
        m_kernel.register_library("cdvdman", 10u, k_search_file_stub) &&
        create_stub(m_kernel.cpu(), k_search_file_stub,
                    k_search_file_syscall);
}

bool c_cdvdman_bridge::ram_pointer(
    uint32_t guest, uint32_t size, uint32_t* physical
)
{
    if (physical == nullptr)
    {
        return false;
    }
    const uint32_t region = guest & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    const uint32_t address = guest & 0x1fffffffu;
    if (address == 0u || address >= c_cpu::k_ram_size ||
        size > c_cpu::k_ram_size - address)
    {
        return false;
    }
    *physical = address;
    return true;
}

bool c_cdvdman_bridge::read_path(
    uint32_t pointer, std::string* path
) const
{
    uint32_t physical = 0u;
    if (path == nullptr || !ram_pointer(pointer, 1u, &physical))
    {
        return false;
    }
    std::string name;
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        if (physical + i >= c_cpu::k_ram_size)
        {
            return false;
        }
        uint32_t word = 0u;
        const uint32_t address = physical + i;
        if (!m_kernel.cpu().read32(address & ~3u, &word))
        {
            return false;
        }
        const uint8_t byte = static_cast<uint8_t>(
            word >> ((address & 3u) * 8u)
        );
        if (byte == 0u)
        {
            if (name.empty())
            {
                return false;
            }
            *path = std::move(name);
            return true;
        }
        if (byte < 0x20u || byte > 0x7eu)
        {
            return false;
        }
        name.push_back(static_cast<char>(byte));
    }
    // A nonterminated path is not a lookup miss: refuse the syscall.
    return false;
}

bool c_cdvdman_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel || service != k_search_file_syscall)
    {
        return false;
    }

    // sceCdSearchFile(sceCdlFILE* output, const char* name)
    // sceCdlFILE = u32 lsn, u32 size, char name[16], u8 date[8].
    const s_cpu_state& state = kernel.cpu().state();
    uint32_t destination = 0u;
    std::string name;
    if ((state.m_gpr[4] & 3u) != 0u ||
        !ram_pointer(state.m_gpr[4], 32u, &destination) ||
        !read_path(state.m_gpr[5], &name))
    {
        return false;
    }

    // cdvdman accepts ISO paths such as "\\SYSTEM.CNF;1", not only
    // LOADFILE's fully qualified "cdrom0:\\..." paths.
    const bool qualified = name.size() >= 7u &&
        (name.substr(0u, 7u) == "cdrom0:" ||
         name.substr(0u, 7u) == "CDROM0:");
    const std::string guest_path = qualified ? name : "cdrom0:" + name;
    s_folder_file_info file{};
    if (!m_source.find_file(guest_path, &file))
    {
        // A missing/unsupported filename is a genuine search miss.
        // Leave the caller's output structure untouched.
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;
    }

    // Probe the complete destination before writing any guest bytes.
    // The bridge writes only ordinary physical IOP RAM, never MMIO.
    uint32_t ignored = 0u;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (!kernel.cpu().read32(destination + i * 4u, &ignored))
        {
            return false;
        }
    }
    uint8_t bytes[32]{};
    put32(bytes, file.m_lba);
    put32(bytes + 4u, file.m_size);
    const std::string file_name = file.m_name + ";1";
    const size_t length = (std::min)(file_name.size(), size_t(15u));
    std::memcpy(bytes + 8u, file_name.data(), length);

    // Match the deterministic ISO9660 directory records emitted by
    // c_folder_sector_source: 2026-01-01 00:00:00, no directory flag.
    // date[] = flags, seconds, minutes, hours, day, month, year LE16.
    bytes[28] = 1u;
    bytes[29] = 1u;
    bytes[30] = 0xeau;
    bytes[31] = 0x07u;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint32_t offset = i * 4u;
        const uint32_t word =
            static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
            (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
            (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
        if (!kernel.cpu().write32(destination + offset, word))
        {
            return false;
        }
    }
    kernel.cpu().state().m_gpr[2] = 1u;
    return true;
}
} // namespace iop
