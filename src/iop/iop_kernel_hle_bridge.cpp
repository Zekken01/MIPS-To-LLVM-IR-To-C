#include "iop_kernel_hle_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace iop
{
namespace
{
struct s_export
{
    const char* m_library;
    uint16_t m_ordinal;
    uint32_t m_family;
};

// Explicit HLE coverage for the actual USA SIO2MAN import families.
// The same services are reusable by later PADMAN/MCMAN/LIBSD IRXs.
constexpr s_export k_exports[] = {
    {"loadcore", 6u, 1u},
    {"intrman", 4u, 2u}, {"intrman", 5u, 2u},
    {"intrman", 6u, 2u}, {"intrman", 7u, 2u},
    {"intrman", 17u, 2u}, {"intrman", 18u, 2u},
    {"stdio", 4u, 3u},
    {"dmacman", 28u, 4u}, {"dmacman", 32u, 4u},
    {"dmacman", 33u, 4u}, {"dmacman", 34u, 4u},
    {"dmacman", 35u, 4u},
    {"thbase", 4u, 5u}, {"thbase", 6u, 5u},
    {"thbase", 20u, 5u},
    {"thevent", 4u, 6u}, {"thevent", 6u, 6u},
    {"thevent", 7u, 6u}, {"thevent", 8u, 6u},
    {"thevent", 10u, 6u},
    {"thsemap", 4u, 7u}, {"thsemap", 6u, 7u},
    {"thsemap", 8u, 7u}, {"thsemap", 11u, 7u},
    {"sysclib", 8u, 8u}, {"sysclib", 29u, 8u},
    {"sysclib", 36u, 8u}
};

bool create_stub(c_cpu& cpu, uint32_t pc, uint32_t service)
{
    const uint32_t words[] = {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0u
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
} // namespace

c_kernel_hle_bridge::c_kernel_hle_bridge(c_kernel& kernel)
    : m_kernel(kernel)
{
}

bool c_kernel_hle_bridge::install()
{
    if (m_installed || m_kernel.rom0_boot_active())
    {
        return false;
    }
    for (uint32_t i = 0u; i < sizeof(k_exports) / sizeof(k_exports[0]); ++i)
    {
        const s_export& item = k_exports[i];
        const uint32_t pc = k_stub_base + i * 0x20u;
        const uint32_t service = k_service_base +
            item.m_family * 0x100u + item.m_ordinal;
        if (!create_stub(m_kernel.cpu(), pc, service) ||
            !m_kernel.register_library(
                item.m_library, item.m_ordinal, pc))
        {
            return false;
        }
    }
    m_installed = true;
    return true;
}

void c_kernel_hle_bridge::reset_for_reboot()
{
    m_interrupts.clear();
    m_semaphores.clear();
    m_events.clear();
    std::fill(std::begin(m_dma_direction),
              std::end(m_dma_direction), 0u);
    m_next_object = 1u;
    m_cpu_interrupts_suspended = false;
    m_installed = false;
}

bool c_kernel_hle_bridge::read_argument(
    uint32_t index, uint32_t* value
) const
{
    if (value == nullptr)
    {
        return false;
    }
    const s_cpu_state& cpu = m_kernel.cpu().state();
    if (index < 4u)
    {
        *value = cpu.m_gpr[4u + index];
        return true;
    }
    const uint64_t address =
        static_cast<uint64_t>(cpu.m_gpr[29]) + 16u +
        static_cast<uint64_t>(index - 4u) * 4u;
    return address <= std::numeric_limits<uint32_t>::max() &&
           m_kernel.cpu().read32(static_cast<uint32_t>(address), value);
}

bool c_kernel_hle_bridge::read_string(
    uint32_t pointer, uint32_t maximum, std::string* output
) const
{
    if (output == nullptr || maximum == 0u ||
        maximum > 65536u)
    {
        return false;
    }
    std::string text;
    for (uint32_t i = 0u; i < maximum; ++i)
    {
        uint8_t ch = 0u;
        if (pointer > std::numeric_limits<uint32_t>::max() - i ||
            !m_kernel.cpu().read8(pointer + i, &ch))
        {
            return false;
        }
        if (ch == 0u)
        {
            *output = std::move(text);
            return true;
        }
        text.push_back(static_cast<char>(ch));
    }
    return false;
}

bool c_kernel_hle_bridge::write_console(
    const std::string& content
)
{
    for (unsigned char ch : content)
    {
        if (!m_kernel.cpu().write8(c_hw::k_debug_console, ch))
        {
            return false;
        }
    }
    return true;
}

bool c_kernel_hle_bridge::register_loaded_guest_library(uint32_t table)
{
    uint32_t marker = 0u;
    if ((table & 3u) != 0u ||
        !m_kernel.cpu().read32(table, &marker) ||
        marker != 0x41c00000u)
    {
        return false;
    }
    std::string name;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        uint8_t ch = 0u;
        if (!m_kernel.cpu().read8(table + 12u + i, &ch))
        {
            return false;
        }
        if (ch == 0u)
        {
            break;
        }
        if (ch < 0x21u || ch > 0x7eu)
        {
            return false;
        }
        name.push_back(static_cast<char>(ch));
    }
    if (name.empty())
    {
        return false;
    }

    // The actual relocated module must have exported these entries.
    // Never create fake providers simply because a guest passes a table.
    bool any = false;
    for (uint32_t ordinal = 0u; ordinal < 1024u; ++ordinal)
    {
        uint32_t entry = 0u;
        if (!m_kernel.cpu().read32(table + 20u + ordinal * 4u,
                                   &entry))
        {
            return false;
        }
        if (entry == 0u)
        {
            return any;
        }
        uint32_t registered = 0u;
        if (!m_kernel.lookup_library(
                name, static_cast<uint16_t>(ordinal), &registered) ||
            registered != entry)
        {
            return false;
        }
        any = true;
    }
    return false;
}

bool c_kernel_hle_bridge::match_event(
    const s_event& event, uint32_t bits, uint32_t mode
) const
{
    return (mode & 1u) != 0u
        ? (event.m_bits & bits) != 0u
        : (event.m_bits & bits) == bits;
}

bool c_kernel_hle_bridge::take_event(
    s_event& event, uint32_t bits, uint32_t mode,
    uint32_t output
)
{
    if (output != 0u &&
        !m_kernel.cpu().write32(output, event.m_bits))
    {
        return false;
    }
    if ((mode & 0x10u) != 0u)
    {
        event.m_bits &= ~bits;
    }
    if ((mode & 0x20u) != 0u)
    {
        event.m_bits = 0u;
    }
    return true;
}

bool c_kernel_hle_bridge::handle_loadcore(uint32_t ordinal)
{
    if (ordinal != 6u ||
        !register_loaded_guest_library(m_kernel.cpu().state().m_gpr[4]))
    {
        return false;
    }
    m_kernel.cpu().state().m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_intrman(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t line = cpu.m_gpr[4];
    if (ordinal == 17u)
    {
        const bool suspended = m_cpu_interrupts_suspended;
        if (line != 0u &&
            !m_kernel.cpu().write32(line, suspended ? 0u : 1u))
        {
            return false;
        }
        m_cpu_interrupts_suspended = true;
        if (!m_kernel.hardware().write(c_hw::k_i_ctrl, 4u, 0u))
        {
            return false;
        }
        cpu.m_gpr[2] = suspended ?
            static_cast<uint32_t>(-102) : 0u;
        return true;
    }
    if (ordinal == 18u)
    {
        m_cpu_interrupts_suspended = line == 0u;
        if (!m_kernel.hardware().write(
                c_hw::k_i_ctrl, 4u,
                m_cpu_interrupts_suspended ? 0u : 1u))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (line >= 32u)
    {
        return false;
    }
    switch (ordinal)
    {
    case 4u:
        if (m_interrupts.find(line) != m_interrupts.end() ||
            (cpu.m_gpr[6] & 3u) != 0u ||
            (cpu.m_gpr[6] & 0x1fffffffu) >= c_cpu::k_ram_size)
        {
            return false;
        }
        m_interrupts.emplace(
            line, s_interrupt{cpu.m_gpr[5],
                              cpu.m_gpr[6], cpu.m_gpr[7]});
        break;
    case 5u:
        if (m_interrupts.erase(line) == 0u)
        {
            return false;
        }
        break;
    case 6u:
    case 7u:
    {
        if (m_interrupts.find(line) == m_interrupts.end())
        {
            return false;
        }
        uint32_t mask = 0u;
        if (!m_kernel.hardware().read(c_hw::k_i_mask, 4u, &mask))
        {
            return false;
        }
        if (ordinal == 6u)
        {
            mask |= 1u << line;
        }
        else
        {
            if (cpu.m_gpr[5] != 0u &&
                !m_kernel.cpu().write32(cpu.m_gpr[5], line))
            {
                return false;
            }
            mask &= ~(1u << line);
        }
        if (!m_kernel.hardware().write(c_hw::k_i_mask, 4u, mask))
        {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_stdio(uint32_t ordinal)
{
    if (ordinal != 4u)
    {
        return false;
    }
    const s_cpu_state& cpu = m_kernel.cpu().state();
    std::string format;
    if (!read_string(cpu.m_gpr[4], 4096u, &format))
    {
        return false;
    }
    std::string output;
    uint32_t argument = 1u;
    for (size_t i = 0u; i < format.size(); ++i)
    {
        if (format[i] != '%')
        {
            output += format[i];
            continue;
        }
        if (++i >= format.size())
        {
            return false;
        }
        if (format[i] == '%')
        {
            output += '%';
            continue;
        }
        bool zero = false;
        if (format[i] == '0')
        {
            zero = true;
            ++i;
        }
        uint32_t width = 0u;
        while (i < format.size() &&
               format[i] >= '0' && format[i] <= '9')
        {
            width = width * 10u + static_cast<uint32_t>(
                format[i++] - '0');
            if (width > 32u)
            {
                return false;
            }
        }
        if (i >= format.size())
        {
            return false;
        }
        uint32_t value = 0u;
        if (!read_argument(argument++, &value))
        {
            return false;
        }
        const char spec = format[i];
        if (spec == 's')
        {
            std::string content;
            if (!read_string(value, 4096u, &content))
            {
                return false;
            }
            output += content;
        }
        else if (spec == 'c')
        {
            output += static_cast<char>(value);
        }
        else if (spec == 'u' || spec == 'd' ||
                 spec == 'x' || spec == 'X' || spec == 'p')
        {
            char buffer[96]{};
            char option[16]{};
            if (width != 0u)
            {
                std::snprintf(option, sizeof(option), "%%%s%u%c",
                              zero ? "0" : "", width,
                              spec == 'p' ? 'x' : spec);
            }
            else
            {
                std::snprintf(option, sizeof(option), "%%%c",
                              spec == 'p' ? 'x' : spec);
            }
            if (spec == 'd')
            {
                std::snprintf(buffer, sizeof(buffer), option,
                              static_cast<int32_t>(value));
            }
            else
            {
                std::snprintf(buffer, sizeof(buffer), option, value);
            }
            output += buffer;
        }
        else
        {
            return false;
        }
        if (output.size() > 8192u)
        {
            return false;
        }
    }
    if (!write_console(output))
    {
        return false;
    }
    m_kernel.cpu().state().m_gpr[2] =
        static_cast<uint32_t>(output.size());
    return true;
}

bool c_kernel_hle_bridge::handle_dmacman(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t channel = cpu.m_gpr[4];
    // The only modeled dmacman channels here are SIO2 DMA11/12.
    // No byte movement/interrupt is synthesized by MMIO registers.
    if (channel != 11u && channel != 12u)
    {
        return false;
    }
    const uint32_t base = channel == 11u
        ? c_hw::k_sio2_dma_rx_base
        : c_hw::k_sio2_dma_tx_base;
    switch (ordinal)
    {
    case 28u:
    {
        uint32_t direction = 0u;
        if (!read_argument(4u, &direction) ||
            !m_kernel.hardware().write(base, 4u, cpu.m_gpr[5]) ||
            !m_kernel.hardware().write(
                base + 4u, 4u,
                (cpu.m_gpr[7] << 16u) | (cpu.m_gpr[6] & 0xffffu)))
        {
            return false;
        }
        m_dma_direction[channel] = direction;
        break;
    }
    case 32u:
        if (!m_kernel.hardware().write(
                base + 8u, 4u,
                0x01000200u | (m_dma_direction[channel] & 1u)))
        {
            return false;
        }
        break;
    case 33u:
    case 34u:
    case 35u:
    {
        const uint32_t shift = (channel - 7u) * 4u;
        uint32_t pcr = 0u;
        if (!m_kernel.hardware().read(
                c_hw::k_dma_pcr2, 4u, &pcr))
        {
            return false;
        }
        const uint32_t mask = 0x0fu << shift;
        if (ordinal == 33u)
        {
            pcr = (pcr & ~mask) |
                ((cpu.m_gpr[5] & 7u) << shift) |
                (pcr & (8u << shift));
        }
        else if (ordinal == 34u)
        {
            pcr |= 8u << shift;
        }
        else
        {
            pcr &= ~(8u << shift);
        }
        if (!m_kernel.hardware().write(c_hw::k_dma_pcr2, 4u, pcr))
        {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 1u;
    return true;
}

bool c_kernel_hle_bridge::handle_thbase(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    switch (ordinal)
    {
    case 4u:
    {
        const uint32_t descriptor = cpu.m_gpr[4];
        uint32_t entry = 0u;
        uint32_t stack = 0u;
        uint32_t priority = 0u;
        if (!m_kernel.cpu().read32(descriptor + 8u, &entry) ||
            !m_kernel.cpu().read32(descriptor + 12u, &stack) ||
            !m_kernel.cpu().read32(descriptor + 16u, &priority))
        {
            return false;
        }
        cpu.m_gpr[2] = m_kernel.create_thread(
            entry, cpu.m_gpr[28], priority, stack);
        return cpu.m_gpr[2] != 0u;
    }
    case 6u:
        if (!m_kernel.start_thread_with_argument(
                cpu.m_gpr[4], cpu.m_gpr[5]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 20u:
        cpu.m_gpr[2] = m_kernel.current_thread_id();
        return true;
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_thevent(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t id = cpu.m_gpr[4];
    if (ordinal == 4u)
    {
        uint32_t attr = 0u;
        uint32_t option = 0u;
        uint32_t initial = 0u;
        if (!m_kernel.cpu().read32(id, &attr) ||
            !m_kernel.cpu().read32(id + 4u, &option) ||
            !m_kernel.cpu().read32(id + 8u, &initial))
        {
            return false;
        }
        const uint32_t created = m_next_object++;
        m_events.emplace(created, s_event{attr, option, initial, {}});
        cpu.m_gpr[2] = created;
        return true;
    }
    auto found = m_events.find(id);
    if (found == m_events.end())
    {
        return false;
    }
    s_event& event = found->second;
    switch (ordinal)
    {
    case 6u:
    case 7u:
        event.m_bits |= cpu.m_gpr[5];
        for (auto it = event.m_waiters.begin();
             it != event.m_waiters.end();)
        {
            if (!match_event(event, it->m_mask, it->m_mode))
            {
                ++it;
                continue;
            }
            if (!take_event(event, it->m_mask,
                            it->m_mode, it->m_output) ||
                !m_kernel.wake_thread(it->m_thread))
            {
                return false;
            }
            it = event.m_waiters.erase(it);
        }
        break;
    case 8u:
        event.m_bits &= cpu.m_gpr[5];
        break;
    case 10u:
    {
        const uint32_t bits = cpu.m_gpr[5];
        const uint32_t mode = cpu.m_gpr[6];
        const uint32_t output = cpu.m_gpr[7];
        if ((mode & ~0x31u) != 0u || bits == 0u)
        {
            return false;
        }
        if (match_event(event, bits, mode))
        {
            if (!take_event(event, bits, mode, output))
            {
                return false;
            }
        }
        else
        {
            if (!m_kernel.block_current())
            {
                return false;
            }
            event.m_waiters.push_back({
                m_kernel.current_thread_id(), bits, mode, output
            });
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_thsemap(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t id = cpu.m_gpr[4];
    if (ordinal == 4u)
    {
        uint32_t attr = 0u;
        uint32_t option = 0u;
        uint32_t count = 0u;
        uint32_t maximum = 0u;
        if (!m_kernel.cpu().read32(id, &attr) ||
            !m_kernel.cpu().read32(id + 4u, &option) ||
            !m_kernel.cpu().read32(id + 8u, &count) ||
            !m_kernel.cpu().read32(id + 12u, &maximum) ||
            maximum == 0u || count > maximum)
        {
            return false;
        }
        const uint32_t created = m_next_object++;
        m_semaphores.emplace(
            created, s_semaphore{attr, option, count, maximum, {}});
        cpu.m_gpr[2] = created;
        return true;
    }
    auto found = m_semaphores.find(id);
    if (found == m_semaphores.end())
    {
        return false;
    }
    s_semaphore& sema = found->second;
    switch (ordinal)
    {
    case 6u:
        if (!sema.m_waiters.empty())
        {
            const uint32_t thread_id = sema.m_waiters.front();
            if (!m_kernel.wake_thread(thread_id))
            {
                return false;
            }
            sema.m_waiters.pop_front();
        }
        else if (sema.m_count < sema.m_maximum)
        {
            ++sema.m_count;
        }
        else
        {
            return false;
        }
        break;
    case 8u:
        if (sema.m_count != 0u)
        {
            --sema.m_count;
        }
        else
        {
            if (!m_kernel.block_current())
            {
                return false;
            }
            sema.m_waiters.push_back(m_kernel.current_thread_id());
        }
        break;
    case 11u:
    {
        const uint32_t output = cpu.m_gpr[5];
        const uint32_t words[] = {
            sema.m_attr, sema.m_option, 0u,
            sema.m_maximum, sema.m_count,
            static_cast<uint32_t>(sema.m_waiters.size())
        };
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            if (!m_kernel.cpu().write32(
                    output + i * 4u, words[i]))
            {
                return false;
            }
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_sysclib(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    if (ordinal == 8u)
    {
        const int ch = static_cast<int>(cpu.m_gpr[4] & 0xffu);
        uint32_t value = 0u;
        if (std::isupper(ch)) value |= 1u;
        if (std::islower(ch)) value |= 2u;
        if (std::isdigit(ch)) value |= 4u;
        if (std::isspace(ch)) value |= 8u;
        if (std::ispunct(ch)) value |= 16u;
        if (std::iscntrl(ch)) value |= 32u;
        if (std::isxdigit(ch)) value |= 64u;
        cpu.m_gpr[2] = value;
        return true;
    }
    if (ordinal == 29u)
    {
        const uint32_t limit = cpu.m_gpr[6];
        if (limit > 65536u)
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        for (uint32_t i = 0u; i < limit; ++i)
        {
            uint8_t lhs = 0u;
            uint8_t rhs = 0u;
            if (!m_kernel.cpu().read8(cpu.m_gpr[4] + i, &lhs) ||
                !m_kernel.cpu().read8(cpu.m_gpr[5] + i, &rhs))
            {
                return false;
            }
            if (lhs != rhs)
            {
                cpu.m_gpr[2] = static_cast<uint32_t>(
                    static_cast<int32_t>(lhs) -
                    static_cast<int32_t>(rhs));
                break;
            }
            if (lhs == 0u)
            {
                break;
            }
        }
        return true;
    }
    if (ordinal == 36u)
    {
        std::string input;
        if (!read_string(cpu.m_gpr[4], 256u, &input))
        {
            return false;
        }
        const uint32_t base = cpu.m_gpr[6];
        if (base != 0u && (base < 2u || base > 36u))
        {
            return false;
        }
        char* end = nullptr;
        const long converted = std::strtol(
            input.c_str(), &end, static_cast<int>(base));
        if (cpu.m_gpr[5] != 0u &&
            !m_kernel.cpu().write32(
                cpu.m_gpr[5],
                cpu.m_gpr[4] +
                    static_cast<uint32_t>(end - input.c_str())))
        {
            return false;
        }
        cpu.m_gpr[2] = static_cast<uint32_t>(converted);
        return true;
    }
    return false;
}

bool c_kernel_hle_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel ||
        service < k_service_base ||
        service >= k_service_base + 0x1000u ||
        !m_installed)
    {
        return false;
    }
    const uint32_t family = (service - k_service_base) >> 8u;
    const uint32_t ordinal = service & 0xffu;
    switch (family)
    {
    case 1u: return handle_loadcore(ordinal);
    case 2u: return handle_intrman(ordinal);
    case 3u: return handle_stdio(ordinal);
    case 4u: return handle_dmacman(ordinal);
    case 5u: return handle_thbase(ordinal);
    case 6u: return handle_thevent(ordinal);
    case 7u: return handle_thsemap(ordinal);
    case 8u: return handle_sysclib(ordinal);
    default: return false;
    }
}
} // namespace iop
