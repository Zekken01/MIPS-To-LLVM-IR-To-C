#include "iop_cdvdman_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
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

uint8_t decimal_to_bcd(uint32_t value)
{
    return static_cast<uint8_t>(((value / 10u) << 4u) | (value % 10u));
}
} // namespace

c_cdvdman_bridge::c_cdvdman_bridge(
    c_kernel& kernel, const c_folder_sector_source& source
) : m_kernel(kernel), m_source(source)
{
}

bool c_cdvdman_bridge::install()
{
    if (!m_source.valid() || m_source.sector_count() == 0u)
    {
        return false;
    }

    // Keep the original #10 address stable. Other CDVDMAN services have
    // distinct executable stubs in the reserved IOP kernel area.
    constexpr std::array<uint16_t, 9u> ordinals = {
        4u, 5u, 8u, 10u, 11u, 12u, 13u, 24u, 28u
    };
    for (uint16_t ordinal : ordinals)
    {
        const uint32_t pc = ordinal == 10u
            ? k_search_file_stub
            : k_extra_stub_base + static_cast<uint32_t>(ordinal) * 0x20u;
        if (!create_stub(m_kernel.cpu(), pc, k_service_base + ordinal) ||
            !m_kernel.register_library("cdvdman", ordinal, pc))
        {
            return false;
        }
    }
    m_initialized = false;
    m_last_error = 0u;
    return true;
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

bool c_cdvdman_bridge::handle_read_clock(uint32_t destination)
{
    uint32_t physical = 0u;
    if (!ram_pointer(destination, 8u, &physical))
    {
        return false;
    }

    // sceCdReadClock returns the eight-byte CDVD RTC layout in BCD.
    // The reference reports Japan Standard Time (UTC+9); deriving the
    // calendar after applying the offset also handles date rollover.
    constexpr std::time_t k_jst_offset = 9 * 60 * 60;
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1))
    {
        return false;
    }
    const std::time_t in_japan = now + k_jst_offset;
    const std::tm* calendar = std::gmtime(&in_japan);
    if (calendar == nullptr)
    {
        return false;
    }
    const uint32_t year = static_cast<uint32_t>(
        (calendar->tm_year + 1900) % 100);
    const std::array<uint8_t, 8u> rtc = {
        0u,
        decimal_to_bcd(static_cast<uint32_t>(calendar->tm_sec)),
        decimal_to_bcd(static_cast<uint32_t>(calendar->tm_min)),
        decimal_to_bcd(static_cast<uint32_t>(calendar->tm_hour)),
        0u,
        decimal_to_bcd(static_cast<uint32_t>(calendar->tm_mday)),
        decimal_to_bcd(static_cast<uint32_t>(calendar->tm_mon + 1)),
        decimal_to_bcd(year)
    };
    for (uint32_t i = 0u; i < rtc.size(); ++i)
    {
        if (!m_kernel.cpu().write8(destination + i, rtc[i]))
        {
            return false;
        }
    }
    return true;
}

bool c_cdvdman_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    s_cpu_state& state = m_kernel.cpu().state();
    switch (service)
    {
    case k_service_base + 4u: // sceCdInit
        // Only the already-mounted verified source is initialised.
        if (!m_source.valid() || m_source.sector_count() == 0u)
        {
            state.m_gpr[2] = 0u;
            return true;
        }
        m_initialized = true;
        m_last_error = 0u;
        state.m_gpr[2] = 1u;
        return true;

    case k_service_base + 5u: // sceCdStandby
        // An extracted, already-mounted DVD requires no physical
        // spindle transition. Acknowledge that idempotent operation,
        // without setting a pending N-command, raising an interrupt,
        // or completing DMA on behalf of the guest.
        state.m_gpr[2] = m_source.valid() &&
            m_source.sector_count() != 0u ? 1u : 0u;
        return true;

    case k_service_base + 8u: // sceCdGetError
        state.m_gpr[2] = m_last_error;
        return true;

    case k_service_base + 11u: // sceCdSync
        // No asynchronous CDVDMAN command exists in this extracted-
        // disc HLE. Sync therefore observes the idle command queue:
        // polling (modes 1/17) and ordinary waiting both return zero.
        // This does not complete any physical CDVD/N-command request.
        state.m_gpr[2] = 0u;
        return true;

    case k_service_base + 12u: // sceCdGetDiskType
        // The mounted SDBZ virtual DVD has one deterministic disc type.
        state.m_gpr[2] = m_source.valid() &&
            m_source.sector_count() != 0u ? 0x14u : 0u;
        return true;

    case k_service_base + 13u: // sceCdDiskReady
        // No native CDVDMAN command can be pending in this HLE subset;
        // this checks the mounted sector backend, not physical hardware.
        state.m_gpr[2] = m_source.valid() &&
            m_source.sector_count() != 0u ? 2u : 0u;
        return true;

    case k_service_base + 24u: // sceCdReadClock
        if (!handle_read_clock(state.m_gpr[4]))
        {
            // Invalid guest output is an unsupported syscall boundary,
            // never a fabricated valid clock.
            return false;
        }
        state.m_gpr[2] = 1u;
        return true;

    case k_service_base + 28u: // sceCdStatus
        state.m_gpr[2] = m_source.valid() &&
            m_source.sector_count() != 0u ? 0x0au : 0u;
        return true;

    case k_search_file_syscall:
        break;
    default:
        return false;
    }

    // sceCdSearchFile(sceCdlFILE* output, const char* name)
    // sceCdlFILE = u32 lsn, u32 size, char name[16], u8 date[8].
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
