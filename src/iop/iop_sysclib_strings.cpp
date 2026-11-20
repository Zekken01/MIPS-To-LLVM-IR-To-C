#include "iop_sysclib_strings.h"
#include "iop_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace iop
{
namespace
{
constexpr uint32_t k_max_string_bytes = 65536u;

bool ram_span(uint32_t address, uint32_t size)
{
    const uint32_t region = address & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    const uint32_t physical = address & 0x1fffffffu;
    return physical <= c_cpu::k_ram_size &&
        size <= c_cpu::k_ram_size - physical;
}

bool read_string(c_cpu& memory, uint32_t address,
                 std::vector<uint8_t>& output)
{
    output.clear();
    for (uint32_t i = 0u; i < k_max_string_bytes; ++i)
    {
        if (!ram_span(address, i + 1u))
        {
            return false;
        }
        uint8_t byte = 0u;
        if (!memory.read8(address + i, &byte))
        {
            return false;
        }
        output.push_back(byte);
        if (byte == 0u)
        {
            return true;
        }
    }
    return false;
}

bool read_prefix(c_cpu& memory, uint32_t address, uint32_t limit,
                 std::vector<uint8_t>& output)
{
    if (limit > k_max_string_bytes)
    {
        return false;
    }
    output.clear();
    for (uint32_t i = 0u; i < limit; ++i)
    {
        if (!ram_span(address, i + 1u))
        {
            return false;
        }
        uint8_t byte = 0u;
        if (!memory.read8(address + i, &byte))
        {
            return false;
        }
        if (byte == 0u)
        {
            break;
        }
        output.push_back(byte);
    }
    return true;
}

bool write_bytes(c_cpu& memory, uint32_t address,
                 const std::vector<uint8_t>& input)
{
    if (input.size() > k_max_string_bytes ||
        !ram_span(address, static_cast<uint32_t>(input.size())))
    {
        return false;
    }
    // All bytes and the full destination are validated first, so rejected
    // writes never modify a prefix of the destination.
    for (uint32_t i = 0u; i < input.size(); ++i)
    {
        if (!memory.write8(address + i, input[i]))
        {
            return false;
        }
    }
    return true;
}

int32_t compare_bytes(uint8_t lhs, uint8_t rhs)
{
    return static_cast<int32_t>(lhs) - static_cast<int32_t>(rhs);
}

bool execute_comparison(c_cpu& memory, uint32_t ordinal,
                        uint32_t lhs_address, uint32_t rhs_address,
                        uint32_t limit, uint32_t* result)
{
    if (result == nullptr || limit > k_max_string_bytes)
    {
        return false;
    }
    // strcmp and strncmp compare one byte at a time and may return before
    // the first terminator. This avoids reading unrelated guest addresses.
    int32_t difference = 0;
    bool resolved = false;
    for (uint32_t i = 0u; i < limit; ++i)
    {
        uint8_t lhs = 0u;
        uint8_t rhs = 0u;
        if (!ram_span(lhs_address, i + 1u) ||
            !ram_span(rhs_address, i + 1u) ||
            !memory.read8(lhs_address + i, &lhs) ||
            !memory.read8(rhs_address + i, &rhs))
        {
            return false;
        }
        difference = compare_bytes(lhs, rhs);
        if (difference != 0 || lhs == 0u)
        {
            resolved = true;
            break;
        }
    }
    // strcmp cannot declare two strings equal after exhausting only the
    // bounded scan limit without observing a terminator.
    if (ordinal == 22u && !resolved)
    {
        return false;
    }
    *result = static_cast<uint32_t>(difference);
    return true;
}

bool execute_search(c_cpu& memory, uint32_t ordinal,
                    uint32_t address, uint32_t argument,
                    uint32_t* result)
{
    std::vector<uint8_t> string;
    if (!read_string(memory, address, string))
    {
        return false;
    }
    uint32_t offset = 0xffffffffu;
    if (ordinal == 21u || ordinal == 25u || ordinal == 26u ||
        ordinal == 32u)
    {
        const uint8_t needle = static_cast<uint8_t>(argument);
        for (uint32_t i = 0u; i < string.size(); ++i)
        {
            if (string[i] == needle)
            {
                offset = i;
                if (ordinal == 21u || ordinal == 25u)
                {
                    break;
                }
            }
        }
    }
    else
    {
        std::vector<uint8_t> needle;
        if (!read_string(memory, argument, needle))
        {
            return false;
        }
        if (ordinal == 31u) // strpbrk
        {
            for (uint32_t i = 0u; i + 1u < string.size(); ++i)
            {
                for (uint32_t j = 0u; j + 1u < needle.size(); ++j)
                {
                    if (string[i] == needle[j])
                    {
                        offset = i;
                        break;
                    }
                }
                if (offset != 0xffffffffu)
                {
                    break;
                }
            }
        }
        else // strstr
        {
            const uint32_t hay_length =
                static_cast<uint32_t>(string.size() - 1u);
            const uint32_t needle_length =
                static_cast<uint32_t>(needle.size() - 1u);
            if (needle_length == 0u)
            {
                offset = 0u;
            }
            else if (needle_length <= hay_length)
            {
                for (uint32_t i = 0u; i <= hay_length - needle_length; ++i)
                {
                    bool matched = true;
                    for (uint32_t j = 0u; j < needle_length; ++j)
                    {
                        if (string[i + j] != needle[j])
                        {
                            matched = false;
                            break;
                        }
                    }
                    if (matched)
                    {
                        offset = i;
                        break;
                    }
                }
            }
        }
    }
    *result = offset == 0xffffffffu ? 0u : address + offset;
    return true;
}

bool execute_span(c_cpu& memory, uint32_t ordinal,
                  uint32_t address, uint32_t set_address,
                  uint32_t* result)
{
    std::vector<uint8_t> string;
    std::vector<uint8_t> characters;
    if (!read_string(memory, address, string) ||
        !read_string(memory, set_address, characters))
    {
        return false;
    }
    std::array<bool, 256> present{};
    for (uint32_t i = 0u; i + 1u < characters.size(); ++i)
    {
        present[characters[i]] = true;
    }
    uint32_t length = 0u;
    for (uint32_t i = 0u; i + 1u < string.size(); ++i)
    {
        if (present[string[i]] != (ordinal == 33u))
        {
            break;
        }
        ++length;
    }
    *result = length;
    return true;
}
} // namespace

bool execute_sysclib_string(c_cpu& memory, uint32_t ordinal)
{
    if (ordinal < 20u || ordinal > 34u)
    {
        return false;
    }
    s_cpu_state& cpu = memory.state();
    const uint32_t first = cpu.m_gpr[4];
    const uint32_t second = cpu.m_gpr[5];
    const uint32_t third = cpu.m_gpr[6];
    uint32_t return_value = 0u;
    bool accepted = false;

    switch (ordinal)
    {
    case 21u: // strchr
    case 25u: // index
    case 26u: // rindex
    case 31u: // strpbrk
    case 32u: // strrchr
    case 34u: // strstr
        accepted = execute_search(memory, ordinal, first,
                                  second, &return_value);
        break;
    case 22u: // strcmp
    case 29u: // strncmp
        accepted = execute_comparison(memory, ordinal,
                                      first, second,
                                      ordinal == 22u ?
                                          k_max_string_bytes : third,
                                      &return_value);
        break;
    case 24u: // strcspn
    case 33u: // strspn
        accepted = execute_span(memory, ordinal, first,
                                second, &return_value);
        break;
    case 27u: // strlen
    {
        std::vector<uint8_t> string;
        accepted = read_string(memory, first, string);
        if (accepted)
        {
            return_value = static_cast<uint32_t>(string.size() - 1u);
        }
        break;
    }
    case 20u: // strcat
    case 23u: // strcpy
    case 28u: // strncat
    case 30u: // strncpy
    {
        if ((ordinal == 28u || ordinal == 30u) &&
            third > k_max_string_bytes)
        {
            break;
        }

        std::vector<uint8_t> source;
        if (ordinal == 28u || ordinal == 30u)
        {
            accepted = read_prefix(memory, second, third, source);
        }
        else
        {
            accepted = read_string(memory, second, source);
        }
        if (!accepted)
        {
            break;
        }

        std::vector<uint8_t> destination;
        if (ordinal == 20u || ordinal == 28u)
        {
            accepted = read_string(memory, first, destination);
            if (!accepted)
            {
                break;
            }
            destination.pop_back(); // replace the original NUL
        }
        if (ordinal == 23u)
        {
            destination = source;
        }
        else if (ordinal == 30u)
        {
            destination = source;
            destination.resize(third, 0u); // strncpy NUL padding
        }
        else
        {
            const size_t required = destination.size() + source.size() +
                (ordinal == 28u ? 1u : 0u);
            if (required > k_max_string_bytes)
            {
                accepted = false;
                break;
            }
            destination.insert(destination.end(), source.begin(), source.end());
            if (ordinal == 28u)
            {
                destination.push_back(0u);
            }
        }
        accepted = write_bytes(memory, first, destination);
        if (accepted)
        {
            return_value = first;
        }
        break;
    }
    default:
        accepted = false;
        break;
    }
    if (accepted)
    {
        cpu.m_gpr[2] = return_value;
    }
    return accepted;
}

namespace
{
constexpr uint32_t k_max_format_bytes = 4096u;
constexpr uint32_t k_max_format_piece = 8192u;
constexpr uint32_t k_max_format_field = 4096u;
constexpr uint32_t k_max_format_arguments = 128u;

struct s_format_arguments
{
    c_cpu& m_memory;
    uint32_t m_ap = 0u;
    uint32_t m_index = 0u;
    bool m_guest_va_list = false;

    bool read32(uint32_t* value)
    {
        if (value == nullptr || m_index >= k_max_format_arguments)
        {
            return false;
        }

        if (m_guest_va_list)
        {
            const uint64_t address = static_cast<uint64_t>(m_ap) +
                static_cast<uint64_t>(m_index) * 4u;
            if (address > std::numeric_limits<uint32_t>::max() ||
                !ram_span(static_cast<uint32_t>(address), 4u) ||
                !m_memory.read32(static_cast<uint32_t>(address), value))
            {
                return false;
            }
        }
        else if (m_index < 4u)
        {
            *value = m_memory.state().m_gpr[4u + m_index];
        }
        else
        {
            const uint64_t address =
                static_cast<uint64_t>(m_memory.state().m_gpr[29]) +
                16u + static_cast<uint64_t>(m_index - 4u) * 4u;
            if (address > std::numeric_limits<uint32_t>::max() ||
                !ram_span(static_cast<uint32_t>(address), 4u) ||
                !m_memory.read32(static_cast<uint32_t>(address), value))
            {
                return false;
            }
        }
        ++m_index;
        return true;
    }

    bool read64(uint64_t* value)
    {
        if (value == nullptr)
        {
            return false;
        }
        // R3000 O32 double/long-long arguments occupy an even pair
        // of 32-bit argument slots. This also advances guest va_list.
        if ((m_index & 1u) != 0u)
        {
            ++m_index;
        }
        uint32_t lo = 0u;
        uint32_t hi = 0u;
        if (!read32(&lo) || !read32(&hi))
        {
            return false;
        }
        *value = (static_cast<uint64_t>(hi) << 32u) | lo;
        return true;
    }
};

template<typename T>
bool append_piece(std::string& output, const std::string& format,
                  T value)
{
    std::array<char, k_max_format_piece> piece{};
    const int written =
        std::snprintf(piece.data(), piece.size(), format.c_str(), value);
    if (written < 0 ||
        static_cast<uint32_t>(written) >= piece.size() ||
        output.size() + static_cast<size_t>(written) >=
            k_max_string_bytes)
    {
        return false;
    }
    output.append(piece.data(), static_cast<size_t>(written));
    return true;
}

bool read_field(const std::string& format, size_t& offset,
                s_format_arguments& arguments, bool precision,
                int32_t* value, bool* specified)
{
    if (value == nullptr || specified == nullptr)
    {
        return false;
    }
    *specified = false;
    *value = 0;
    if (offset < format.size() && format[offset] == '*')
    {
        uint32_t word = 0u;
        if (!arguments.read32(&word))
        {
            return false;
        }
        ++offset;
        const int64_t signed_value =
            static_cast<int32_t>(word);
        if (signed_value < -static_cast<int64_t>(k_max_format_field) ||
            signed_value > static_cast<int64_t>(k_max_format_field))
        {
            return false;
        }
        *value = static_cast<int32_t>(signed_value);
        *specified = !precision || signed_value >= 0;
        return true;
    }

    while (offset < format.size() &&
           format[offset] >= '0' && format[offset] <= '9')
    {
        *specified = true;
        const int32_t digit = format[offset] - '0';
        if (*value > static_cast<int32_t>(k_max_format_field / 10u) ||
            *value * 10 + digit >
                static_cast<int32_t>(k_max_format_field))
        {
            return false;
        }
        *value = *value * 10 + digit;
        ++offset;
    }
    return true;
}

bool render_guest_format(c_cpu& memory, const std::string& format,
                         s_format_arguments& arguments,
                         std::string& output)
{
    output.clear();
    for (size_t index = 0u; index < format.size(); ++index)
    {
        if (format[index] != '%')
        {
            output.push_back(format[index]);
            if (output.size() >= k_max_string_bytes)
            {
                return false;
            }
            continue;
        }

        ++index;
        if (index >= format.size())
        {
            return false;
        }
        if (format[index] == '%')
        {
            output.push_back('%');
            continue;
        }

        std::string flags;
        while (index < format.size() &&
               std::strchr("-+ #0", format[index]) != nullptr)
        {
            if (flags.find(format[index]) == std::string::npos)
            {
                flags.push_back(format[index]);
            }
            ++index;
        }

        int32_t width = 0;
        bool width_specified = false;
        if (!read_field(format, index, arguments, false,
                        &width, &width_specified))
        {
            return false;
        }
        if (width_specified && width < 0)
        {
            width = -width;
            if (flags.find('-') == std::string::npos)
            {
                flags.push_back('-');
            }
        }

        int32_t precision = 0;
        bool precision_specified = false;
        if (index < format.size() && format[index] == '.')
        {
            ++index;
            precision_specified = true;
            bool digits = false;
            if (!read_field(format, index, arguments, true,
                            &precision, &digits))
            {
                return false;
            }
            if (precision < 0)
            {
                precision_specified = false;
            }
        }

        uint32_t length = 0u;
        if (index < format.size() &&
            (format[index] == 'h' || format[index] == 'l'))
        {
            const char qualifier = format[index++];
            length = qualifier == 'h' ? 1u : 3u;
            if (index < format.size() &&
                format[index] == qualifier)
            {
                ++index;
                length = qualifier == 'h' ? 2u : 4u;
            }
        }
        if (index >= format.size())
        {
            return false;
        }
        const char conversion = format[index];
        if (std::strchr("diuoxXcspfFeEgGaA", conversion) == nullptr)
        {
            // In particular %n may never write to arbitrary guest RAM.
            // Unknown conversions also fail without a partial sprintf.
            return false;
        }
        if (length != 0u &&
            (conversion == 's' || conversion == 'c' ||
             conversion == 'p'))
        {
            return false;
        }

        std::string spec = "%" + flags;
        if (width_specified)
        {
            spec += std::to_string(width);
        }
        if (precision_specified)
        {
            spec += "." + std::to_string(precision);
        }
        if (length == 4u)
        {
            spec += "ll";
        }
        spec += conversion;

        uint32_t raw = 0u;
        if (conversion == 's')
        {
            if (!arguments.read32(&raw) || raw == 0u)
            {
                return false;
            }
            std::vector<uint8_t> chars;
            if (precision_specified)
            {
                if (!read_prefix(memory, raw,
                                 static_cast<uint32_t>(precision),
                                 chars))
                {
                    return false;
                }
            }
            else if (!read_string(memory, raw, chars))
            {
                return false;
            }
            if (!precision_specified)
            {
                chars.pop_back();
            }
            const std::string guest(chars.begin(), chars.end());
            if (!append_piece(output, spec, guest.c_str()))
            {
                return false;
            }
        }
        else if (conversion == 'c')
        {
            if (!arguments.read32(&raw) ||
                !append_piece(output, spec, static_cast<int>(raw)))
            {
                return false;
            }
        }
        else if (conversion == 'p')
        {
            // The primary ps2-web-recomp SDBZ reference renders
            // guest 32-bit pointers as eight lowercase hex digits.
            if (!arguments.read32(&raw) ||
                !append_piece(output, "%08x", raw))
            {
                return false;
            }
        }
        else if (std::strchr("fFeEgGaA", conversion) != nullptr)
        {
            if (length != 0u && length != 3u)
            {
                return false;
            }
            uint64_t bits = 0u;
            double value = 0.0;
            if (!arguments.read64(&bits))
            {
                return false;
            }
            std::memcpy(&value, &bits, sizeof(value));
            if (!append_piece(output, spec, value))
            {
                return false;
            }
        }
        else if (length == 4u)
        {
            uint64_t value = 0u;
            if (!arguments.read64(&value))
            {
                return false;
            }
            if (conversion == 'd' || conversion == 'i')
            {
                if (!append_piece(output, spec,
                                  static_cast<long long>(
                                      static_cast<int64_t>(value))))
                {
                    return false;
                }
            }
            else if (!append_piece(output, spec,
                                   static_cast<unsigned long long>(value)))
            {
                return false;
            }
        }
        else
        {
            if (!arguments.read32(&raw))
            {
                return false;
            }
            if (length == 1u)
            {
                raw = (conversion == 'd' || conversion == 'i')
                    ? static_cast<uint32_t>(
                        static_cast<int16_t>(raw))
                    : static_cast<uint16_t>(raw);
            }
            else if (length == 2u)
            {
                raw = (conversion == 'd' || conversion == 'i')
                    ? static_cast<uint32_t>(
                        static_cast<int8_t>(raw))
                    : static_cast<uint8_t>(raw);
            }
            if ((conversion == 'd' || conversion == 'i')
                ? !append_piece(
                    output, spec, static_cast<int32_t>(raw))
                : !append_piece(output, spec, raw))
            {
                return false;
            }
        }
    }
    return output.size() < k_max_string_bytes;
}
} // namespace

bool execute_sysclib_format(c_cpu& memory, uint32_t ordinal)
{
    if (ordinal != 19u && ordinal != 42u)
    {
        return false;
    }
    s_cpu_state& cpu = memory.state();
    const uint32_t destination = cpu.m_gpr[4];
    const uint32_t format_address = cpu.m_gpr[5];
    std::vector<uint8_t> format_bytes;
    if (destination == 0u || format_address == 0u ||
        !ram_span(destination, 1u) ||
        !read_string(memory, format_address, format_bytes) ||
        format_bytes.size() > k_max_format_bytes + 1u)
    {
        return false;
    }
    const std::string format(
        format_bytes.begin(), format_bytes.end() - 1);
    s_format_arguments args{memory};
    args.m_guest_va_list = ordinal == 42u;
    args.m_index = ordinal == 19u ? 2u : 0u;
    args.m_ap = cpu.m_gpr[6];
    if (args.m_guest_va_list &&
        (args.m_ap == 0u || (args.m_ap & 3u) != 0u ||
         !ram_span(args.m_ap, 4u)))
    {
        return false;
    }
    std::string output;
    if (!render_guest_format(memory, format, args, output) ||
        output.size() >= k_max_string_bytes ||
        !ram_span(destination, static_cast<uint32_t>(output.size() + 1u)))
    {
        return false;
    }
    std::vector<uint8_t> bytes(output.begin(), output.end());
    bytes.push_back(0u);
    if (!write_bytes(memory, destination, bytes))
    {
        return false;
    }
    cpu.m_gpr[2] = static_cast<uint32_t>(output.size());
    return true;
}
} // namespace iop
