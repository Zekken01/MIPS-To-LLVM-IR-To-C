#include "iop_kernel.h"
#include "iop_kernel_hle_bridge.h"

#include <cstdint>
#include <cstdio>

namespace
{
bool check(bool passed, const char* message)
{
    if (!passed)
    {
        std::fprintf(stderr, "iop_sysclib_string_test: %s\n", message);
    }
    return passed;
}

bool write_string(iop::c_cpu& memory, uint32_t address, const char* string)
{
    uint32_t i = 0u;
    while (string[i] != 0)
    {
        if (!memory.write8(address + i, static_cast<uint8_t>(string[i])))
        {
            return false;
        }
        ++i;
    }
    return memory.write8(address + i, 0u);
}

bool read_char(iop::c_cpu& memory, uint32_t address, uint8_t expected)
{
    uint8_t value = 0u;
    return memory.read8(address, &value) && value == expected;
}
}

int main()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!check(hle.install(), "SYSCLIB exports failed to install"))
    {
        return 1;
    }
    auto& memory = kernel.cpu();
    auto& cpu = memory.state();
    const auto invoke = [&](uint32_t ordinal, uint32_t a0,
                            uint32_t a1, uint32_t a2) -> bool
    {
        cpu.m_gpr[4] = a0;
        cpu.m_gpr[5] = a1;
        cpu.m_gpr[6] = a2;
        return hle.handle_syscall(
            kernel, iop::c_kernel_hle_bridge::k_service_base +
                0x800u + ordinal);
    };
    for (uint16_t ordinal = 20u; ordinal <= 34u; ++ordinal)
    {
        uint32_t entry = 0u;
        if (!check(kernel.lookup_library("sysclib", ordinal, &entry) &&
                   entry != 0u, "missing documented SYSCLIB string export"))
        {
            return 1;
        }
    }
    uint32_t missing = 0u;
    if (!check(!kernel.lookup_library("sysclib", 35u, &missing),
               "stateful strtok must remain unimplemented"))
    {
        return 1;
    }

    if (!write_string(memory, 0x4000u, "banana") ||
        !write_string(memory, 0x4100u, "ana") ||
        !write_string(memory, 0x4200u, "abc") ||
        !write_string(memory, 0x4300u, "bny") ||
        !write_string(memory, 0x4400u, "abcdef") ||
        !write_string(memory, 0x4500u, "bananb") ||
        !write_string(memory, 0x4600u, "") ||
        !write_string(memory, 0x4700u, "xx"))
    {
        return 1;
    }

    if (!check(invoke(27u, 0x80004000u, 0u, 0u) &&
               cpu.m_gpr[2] == 6u,
               "strlen must return length without terminator") ||
        !check(invoke(21u, 0x4000u, 'a', 0u) &&
               cpu.m_gpr[2] == 0x4001u,
               "strchr must return first occurrence") ||
        !check(invoke(25u, 0x4000u, 'n', 0u) &&
               cpu.m_gpr[2] == 0x4002u,
               "index alias must return first occurrence") ||
        !check(invoke(26u, 0x4000u, 'a', 0u) &&
               cpu.m_gpr[2] == 0x4005u,
               "rindex alias must return last occurrence") ||
        !check(invoke(32u, 0x4000u, 0u, 0u) &&
               cpu.m_gpr[2] == 0x4006u,
               "strrchr NUL must point at terminator") ||
        !check(invoke(21u, 0x4000u, 'z', 0u) &&
               cpu.m_gpr[2] == 0u,
               "strchr missing character must return null") ||
        !check(invoke(34u, 0x4000u, 0x4100u, 0u) &&
               cpu.m_gpr[2] == 0x4001u,
               "strstr must find first matching substring") ||
        !check(invoke(34u, 0x4000u, 0x4600u, 0u) &&
               cpu.m_gpr[2] == 0x4000u,
               "strstr empty needle must return haystack") ||
        !check(invoke(31u, 0x4000u, 0x4300u, 0u) &&
               cpu.m_gpr[2] == 0x4000u,
               "strpbrk must return first character from set"))
    {
        return 1;
    }
    if (!check(invoke(24u, 0x4000u, 0x4100u, 0u) &&
               cpu.m_gpr[2] == 1u,
               "strcspn must count leading excluded characters") ||
        !check(invoke(33u, 0x4400u, 0x4200u, 0u) &&
               cpu.m_gpr[2] == 3u,
               "strspn must count leading accepted characters") ||
        !check(invoke(22u, 0x4000u, 0x4500u, 0u) &&
               static_cast<int32_t>(cpu.m_gpr[2]) == -1,
               "strcmp must compare unsigned bytes") ||
        !check(invoke(29u, 0x4000u, 0x4500u, 5u) &&
               cpu.m_gpr[2] == 0u,
               "strncmp must honor length bound") ||
        !check(invoke(29u, 0x4000u, 0x4500u, 6u) &&
               static_cast<int32_t>(cpu.m_gpr[2]) == -1,
               "strncmp must compare final byte") ||
        !check(invoke(29u, 0xffffffffu, 0xdeadbeefu, 0u) &&
               cpu.m_gpr[2] == 0u,
               "zero-count strncmp must not access invalid pointers"))
    {
        return 1;
    }

    if (!check(invoke(23u, 0xa0004800u, 0x4000u, 0u) &&
               cpu.m_gpr[2] == 0xa0004800u &&
               read_char(memory, 0x4800u, 'b') &&
               read_char(memory, 0x4805u, 'a') &&
               read_char(memory, 0x4806u, 0u),
               "strcpy must copy terminator and return original pointer") ||
        !check(invoke(20u, 0x4800u, 0x4100u, 0u) &&
               cpu.m_gpr[2] == 0x4800u &&
               read_char(memory, 0x4806u, 'a') &&
               read_char(memory, 0x4808u, 'a') &&
               read_char(memory, 0x4809u, 0u),
               "strcat must replace terminal NUL and append source") ||
        !check(invoke(28u, 0x4800u, 0x4700u, 1u) &&
               cpu.m_gpr[2] == 0x4800u &&
               read_char(memory, 0x4809u, 'x') &&
               read_char(memory, 0x480au, 0u),
               "strncat must append at most N bytes and a NUL"))
    {
        return 1;
    }

    if (!memory.write8(0x4906u, 0x55u) ||
        !check(invoke(30u, 0x4900u, 0x4100u, 6u) &&
               cpu.m_gpr[2] == 0x4900u &&
               read_char(memory, 0x4900u, 'a') &&
               read_char(memory, 0x4902u, 'a') &&
               read_char(memory, 0x4903u, 0u) &&
               read_char(memory, 0x4905u, 0u) &&
               read_char(memory, 0x4906u, 0x55u),
               "strncpy must pad NULs without overwriting next byte") ||
        !check(invoke(30u, 0x4a00u, 0x4000u, 2u) &&
               read_char(memory, 0x4a00u, 'b') &&
               read_char(memory, 0x4a01u, 'a'),
               "strncpy truncation must not require a source terminator"))
    {
        return 1;
    }

    // No partially written output for crossing RAM, ROM or MMIO boundaries.
    if (!memory.write8(0x001ffffeu, 0x5au) ||
        !memory.write8(0x001fffffu, 0x6bu))
    {
        return 1;
    }
    cpu.m_gpr[2] = 0x1234u;
    if (!check(!invoke(23u, 0x001ffffeu, 0x4100u, 0u) &&
               read_char(memory, 0x001ffffeu, 0x5au) &&
               read_char(memory, 0x001fffffu, 0x6bu) &&
               cpu.m_gpr[2] == 0x1234u,
               "strcpy crossing RAM must fail transactionally") ||
        !check(!invoke(20u, 0x1fc00000u, 0x4100u, 0u),
               "strcat ROM destination must be rejected") ||
        !check(!invoke(27u, 0x10000000u, 0u, 0u),
               "strlen must reject MMIO pointer") ||
        !check(!invoke(30u, 0x4800u, 0x4000u, 0xffffffffu) &&
               read_char(memory, 0x4800u, 'b'),
               "oversized strncpy must not touch destination") ||
        !check(!invoke(35u, 0x4000u, 0x4100u, 0u),
               "stateful strtok must fail closed"))
    {
        return 1;
    }

    std::puts("iop_sysclib_string_test: PASS");
    return 0;
}
