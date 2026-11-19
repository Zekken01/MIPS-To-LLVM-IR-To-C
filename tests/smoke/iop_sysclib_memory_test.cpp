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
        std::fprintf(stderr, "iop_sysclib_memory_test: %s\n", message);
    }
    return passed;
}
}

int main()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!check(hle.install(), "could not install SYSCLIB HLE"))
    {
        return 1;
    }

    const auto invoke = [&](uint32_t ordinal, uint32_t first,
                            uint32_t second, uint32_t third) -> bool
    {
        iop::s_cpu_state& cpu = kernel.cpu().state();
        cpu.m_gpr[4] = first;
        cpu.m_gpr[5] = second;
        cpu.m_gpr[6] = third;
        return hle.handle_syscall(
            kernel,
            iop::c_kernel_hle_bridge::k_service_base + 0x800u + ordinal
        );
    };
    const auto read = [&](uint32_t address, uint8_t expected) -> bool
    {
        uint8_t value = 0u;
        return kernel.cpu().read8(address, &value) && value == expected;
    };
    const auto write = [&](uint32_t address, uint8_t value) -> bool
    {
        return kernel.cpu().write8(address, value);
    };

    for (uint32_t ordinal = 10u; ordinal <= 17u; ++ordinal)
    {
        uint32_t entry = 0u;
        if (!check(kernel.lookup_library("sysclib",
                                          static_cast<uint16_t>(ordinal),
                                          &entry) && entry != 0u,
                   "real SYSCLIB memory ordinal not registered"))
        {
            return 1;
        }
    }
    if (!check(!kernel.lookup_library("sysclib", 9u, nullptr),
               "unsupported SYSCLIB ordinal unexpectedly registered"))
    {
        return 1;
    }

    for (uint32_t i = 0u; i < 5u; ++i)
    {
        if (!write(0x3100u + i, static_cast<uint8_t>(0x10u + i * 0x10u)) ||
            !write(0x3000u + i, 0xccu))
        {
            return 1;
        }
    }
    if (!check(invoke(12u, 0x80003000u, 0xa0003100u, 5u),
               "KSEG0/KSEG1 memcpy failed") ||
        !check(kernel.cpu().state().m_gpr[2] == 0x80003000u,
               "memcpy did not return destination") ||
        !check(read(0x3000u, 0x10u) && read(0x3004u, 0x50u),
               "memcpy did not copy source bytes"))
    {
        return 1;
    }

    if (!check(invoke(13u, 0x3001u, 0x3000u, 4u),
               "overlapping memmove failed") ||
        !check(read(0x3000u, 0x10u) && read(0x3001u, 0x10u) &&
               read(0x3002u, 0x20u) && read(0x3003u, 0x30u) &&
               read(0x3004u, 0x40u),
               "memmove did not snapshot overlapping input"))
    {
        return 1;
    }
    if (!check(invoke(10u, 0x3000u, 0x30u, 5u) &&
               kernel.cpu().state().m_gpr[2] == 0x3003u,
               "memchr returned incorrect guest address"))
    {
        return 1;
    }

    if (!write(0x3200u, 0x10u) || !write(0x3201u, 0x10u) ||
        !write(0x3202u, 0x21u))
    {
        return 1;
    }
    if (!check(invoke(11u, 0x3000u, 0x3200u, 3u) &&
               static_cast<int32_t>(kernel.cpu().state().m_gpr[2]) == -1,
               "memcmp must return unsigned-byte difference") ||
        !check(invoke(15u, 0x3000u, 0x3200u, 3u) &&
               kernel.cpu().state().m_gpr[2] != 0u,
               "bcmp must distinguish unequal ranges"))
    {
        return 1;
    }
    if (!check(invoke(14u, 0x3000u, 0x5au, 3u) &&
               kernel.cpu().state().m_gpr[2] == 0x3000u &&
               read(0x3000u, 0x5au) && read(0x3002u, 0x5au),
               "memset returned/wrote incorrect bytes"))
    {
        return 1;
    }
    kernel.cpu().state().m_gpr[2] = 0xaabbccddu;
    if (!check(invoke(16u, 0x3200u, 0x3300u, 3u) &&
               read(0x3300u, 0x10u) && read(0x3302u, 0x21u) &&
               kernel.cpu().state().m_gpr[2] == 0xaabbccddu,
               "bcopy must copy bytes without inventing a return value"))
    {
        return 1;
    }
    if (!check(invoke(17u, 0x3000u, 2u, 0u) &&
               read(0x3000u, 0u) && read(0x3001u, 0u) &&
               read(0x3002u, 0x5au) &&
               kernel.cpu().state().m_gpr[2] == 0xaabbccddu,
               "bzero must zero its range without returning a value"))
    {
        return 1;
    }

    // Reject whole invalid spans before any destination byte changes.
    if (!check(!invoke(12u, 0x3300u, 0x001fffffu, 3u) &&
               read(0x3300u, 0x10u) && read(0x3302u, 0x21u),
               "invalid source partially modified destination") ||
        !check(!invoke(13u, 0x001fffffu, 0x3200u, 3u) &&
               read(0x001fffffu, 0u),
               "invalid destination was accepted") ||
        !check(!invoke(14u, 0x3300u, 0xffu, 0xffffffffu) &&
               read(0x3300u, 0x10u),
               "oversized length wrote to RAM") ||
        !check(!invoke(12u, 0x3300u, 0x1fc00000u, 3u) &&
               read(0x3300u, 0x10u),
               "ROM/MMIO source was treated as writable RAM"))
    {
        return 1;
    }
    if (!check(invoke(12u, 0xdeadbeefu, 0xffffffffu, 0u) &&
               kernel.cpu().state().m_gpr[2] == 0xdeadbeefu,
               "zero-byte memcpy must not dereference addresses") ||
        !check(!invoke(9u, 0x3000u, 0u, 0u),
               "unknown SYSCLIB ordinal must fail closed"))
    {
        return 1;
    }

    std::puts("iop_sysclib_memory_test: PASS");
    return 0;
}
