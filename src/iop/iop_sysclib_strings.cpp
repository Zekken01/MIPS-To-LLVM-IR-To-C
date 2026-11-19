#include "iop_sysclib_strings.h"
#include "iop_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
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
} // namespace iop
