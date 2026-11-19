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
        std::fprintf(stderr, "iop_sysclib_ctype_test: %s\n", message);
    }
    return passed;
}
}

int main()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!check(hle.install(), "failed to install IOP SYSCLIB"))
    {
        return 1;
    }
    auto& memory = kernel.cpu();
    auto& cpu = memory.state();
    const auto invoke = [&](uint32_t ordinal, uint32_t ch) -> bool
    {
        cpu.m_gpr[4] = ch;
        return hle.handle_syscall(
            kernel, iop::c_kernel_hle_bridge::k_service_base +
                0x800u + ordinal);
    };

    for (uint16_t ordinal = 6u; ordinal <= 9u; ++ordinal)
    {
        uint32_t entry = 0u;
        if (!check(kernel.lookup_library("sysclib", ordinal, &entry) &&
                   entry != 0u, "missing SYSCLIB ctype export"))
        {
            return 1;
        }
    }
    if (!check(invoke(6u, 'z') && cpu.m_gpr[2] == 'Z',
               "_toupper must convert ASCII lower-case") ||
        !check(invoke(6u, 'M') && cpu.m_gpr[2] == 'M',
               "_toupper must preserve uppercase") ||
        !check(invoke(7u, 'A') && cpu.m_gpr[2] == 'a',
               "_tolower must convert ASCII uppercase") ||
        !check(invoke(7u, 'x') && cpu.m_gpr[2] == 'x',
               "_tolower must preserve lowercase"))
    {
        return 1;
    }

    struct s_expected
    {
        uint32_t m_character;
        uint8_t m_flags;
    };
    const s_expected expected[] = {
        {0u, 0x20u}, {9u, 0x08u}, {10u, 0x08u},
        {13u, 0x08u}, {14u, 0x20u}, {' ', 0x18u},
        {'!', 0x10u}, {'0', 0x04u}, {'9', 0x04u},
        {':', 0x10u}, {'A', 0x41u}, {'F', 0x41u},
        {'G', 0x01u}, {'Z', 0x01u}, {'a', 0x42u},
        {'f', 0x42u}, {'g', 0x02u}, {'z', 0x02u},
        {'~', 0x10u}, {127u, 0x20u}
    };
    if (!check(invoke(9u, 0u), "get_ctype_table syscall failed"))
    {
        return 1;
    }
    const uint32_t table_address = cpu.m_gpr[2];
    if (!check(table_address != 0u &&
               table_address < iop::c_cpu::k_ram_size - 128u,
               "ctype table must be a real guest IOP RAM pointer"))
    {
        return 1;
    }
    for (const s_expected& sample : expected)
    {
        uint8_t table_value = 0u;
        if (!check(invoke(8u, sample.m_character) &&
                   cpu.m_gpr[2] == sample.m_flags,
                   "look_ctype_table mismatches real PS2SDK classification") ||
            !check(memory.read8(table_address + sample.m_character,
                                &table_value) &&
                   table_value == sample.m_flags,
                   "get_ctype_table pointer contains incorrect guest byte"))
        {
            return 1;
        }
    }

    cpu.m_gpr[2] = 0xfaceb00cu;
    if (!check(!invoke(8u, 128u) &&
               cpu.m_gpr[2] == 0xfaceb00cu,
               "out-of-range ctype input must fail closed"))
    {
        return 1;
    }

    std::puts("iop_sysclib_ctype_test: PASS");
    return 0;
}
