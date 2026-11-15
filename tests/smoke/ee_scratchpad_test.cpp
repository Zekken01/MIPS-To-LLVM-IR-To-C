#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE scratchpad memory: %s\n", reason);
    return 1;
}
} // namespace

int main()
{
    ee::ee_memory memory(4096);
    constexpr uint32_t base = ee::ee_memory::k_scratchpad_base;
    constexpr uint32_t size = static_cast<uint32_t>(ee::ee_memory::k_scratchpad_size);

    // PS2 scratchpad is a distinct 16 KiB region, not a mirror of main RAM.
    if (!memory.contains(base, 1) ||
        !memory.contains(base + size - 16u, 16) ||
        memory.contains(base + size - 1u, 2) ||
        memory.contains(base + size, 1) ||
        memory.contains(base - 1u, 2))
    {
        return fail("address bounds incorrect");
    }

    memory.set_ram_range(0, 4096);
    memory.write32(0, 0xdeadbeefu);
    memory.write8(base, 0x12u);
    memory.write16(base + 2u, 0x3456u);
    memory.write32(base + 4u, 0x78563412u);
    memory.write64(base + 8u, 0x1122334455667788ull);

    if (memory.read8(base) != 0x12u ||
        memory.read16(base + 2u) != 0x3456u ||
        memory.read32(base + 4u) != 0x78563412u ||
        memory.read64(base + 8u) != 0x1122334455667788ull ||
        memory.read8(base + 2u) != 0x56u ||
        memory.read8(base + 3u) != 0x34u ||
        memory.read32(0) != 0xdeadbeefu)
    {
        return fail("scalar loads/stores or RAM isolation failed");
    }

    const ee::s_ee_u128 quad{0x0102030405060708ull, 0x1112131415161718ull};
    const uint32_t last_quad = base + size - 16u;
    memory.write128(last_quad, &quad);
    ee::s_ee_u128 loaded{};
    memory.read128(last_quad, &loaded);
    if (loaded.m_lo != quad.m_lo || loaded.m_hi != quad.m_hi)
    {
        return fail("128-bit round trip at end of scratchpad failed");
    }

    // The full access must be in range. A crossing store may not write the
    // valid portion before discovering that the remainder is unmapped.
    memory.write64(base + size - 4u, 0xffffffffffffffffull);
    memory.write128(base + size - 8u, &quad);
    memory.write16(base + size - 1u, 0xffffu);
    memory.write32(base + size - 2u, 0xffffffffu);
    if (memory.read128(last_quad, &loaded),
        loaded.m_lo != quad.m_lo || loaded.m_hi != quad.m_hi ||
        memory.read16(base + size - 1u) != 0 ||
        memory.read64(base + size - 4u) != 0 ||
        memory.read32(base + size) != 0)
    {
        return fail("partial or out-of-bounds accesses modified scratchpad");
    }

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    ee::ee_mem_write32(&runtime, base + 0x100u, 0xcafebabeu);
    if (ee::ee_mem_read32(&runtime, base + 0x100u) != 0xcafebabeu)
    {
        return fail("generated-code memory helper ABI did not map scratchpad");
    }

    memory.set_kseg_ram_aliases(true);
    memory.write32(0xa0000000u, 0x12345678u);
    if (memory.read32(0x80000000u) != 0x12345678u ||
        memory.read32(base + 4u) != 0x78563412u ||
        memory.read32(0) != 0x12345678u)
    {
        return fail("scratchpad and KSEG main RAM aliasing interfered");
    }

    memory.set_kseg_ram_aliases(false);
    if (!memory.contains(base, 4) || memory.contains(0x80000000u, 4))
    {
        return fail("scratchpad unexpectedly depends on KSEG alias mode");
    }

    std::puts("EE scratchpad 16 KiB / width / bounds / ABI: PASS");
    return 0;
}
