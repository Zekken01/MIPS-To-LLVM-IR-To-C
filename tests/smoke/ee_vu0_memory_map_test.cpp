#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_vu0_code = 0x11000000u;
constexpr uint32_t k_vu0_data = 0x11004000u;
constexpr uint32_t k_vu0_size = 0x1000u;

int fail(const char* what)
{
    std::fprintf(stderr, "EE VU0 memory aperture: %s\n", what);
    return 1;
}

bool check_bank(ee::s_ee_runtime* runtime, uint8_t* backing, uint32_t base)
{
    ee::ee_mem_write8(runtime, base, 0x80u);
    ee::ee_mem_write16(runtime, base + 2u, 0x1234u);
    ee::ee_mem_write32(runtime, base + 4u, 0xdeadbeefu);
    ee::ee_mem_write64(runtime, base + 8u, 0x1122334455667788ull);

    if (ee::ee_mem_read8(runtime, base) != 0x80u ||
        ee::ee_mem_read16(runtime, base + 2u) != 0x1234u ||
        ee::ee_mem_read32(runtime, base + 4u) != 0xdeadbeefu ||
        ee::ee_mem_read64(runtime, base + 8u) != 0x1122334455667788ull)
    {
        return false;
    }

    if (backing[0] != 0x80u || backing[2] != 0x34u ||
        backing[3] != 0x12u || backing[4] != 0xefu ||
        backing[7] != 0xdeu || backing[8] != 0x88u ||
        backing[15] != 0x11u)
    {
        return false;
    }

    ee::s_ee_u128 quad{};
    quad.m_lo = 0x0102030405060708ull;
    quad.m_hi = 0xf1f2f3f4f5f6f7f8ull;
    ee::ee_mem_write128(runtime, base + k_vu0_size - 16u, &quad);

    ee::s_ee_u128 loaded{};
    ee::ee_mem_read128(runtime, base + k_vu0_size - 16u, &loaded);
    if (loaded.m_lo != quad.m_lo || loaded.m_hi != quad.m_hi)
    {
        return false;
    }

    // Neither writes nor reads may spill into the unmapped tail, or into
    // any unrelated EE region. Cross-boundary writes are atomic rejections.
    ee::ee_mem_write64(runtime, base + k_vu0_size - 4u, ~uint64_t{0});
    ee::ee_mem_write128(runtime, base + k_vu0_size - 8u, &quad);
    ee::ee_mem_write16(runtime, base + k_vu0_size - 1u, 0xffffu);
    ee::ee_mem_write32(runtime, base + k_vu0_size - 2u, 0xffffffffu);
    ee::ee_mem_read128(runtime, base + k_vu0_size - 16u, &loaded);
    if (loaded.m_lo != quad.m_lo || loaded.m_hi != quad.m_hi ||
        ee::ee_mem_read64(runtime, base + k_vu0_size - 4u) != 0 ||
        ee::ee_mem_read16(runtime, base + k_vu0_size - 1u) != 0 ||
        ee::ee_mem_read32(runtime, base + k_vu0_size) != 0)
    {
        return false;
    }

    loaded.m_lo = ~uint64_t{0};
    loaded.m_hi = ~uint64_t{0};
    ee::ee_mem_read128(runtime, base + k_vu0_size - 8u, &loaded);
    return loaded.m_lo == 0 && loaded.m_hi == 0;
}
} // namespace

int main()
{
    ee::ee_memory ram(4096);
    ee::s_ee_vu0_state vu0{};
    ee::s_ee_runtime runtime{};
    runtime.m_user = &ram;
    runtime.m_vu0_state = &vu0;

    ram.write32(0x20u, 0xabcdef12u);
    if (!check_bank(&runtime, vu0.m_micro_memory, k_vu0_code) ||
        !check_bank(&runtime, vu0.m_data_memory, k_vu0_data))
    {
        return fail("scalar/quadword map, byte order, or bounds");
    }

    ee::ee_mem_write32(&runtime, k_vu0_data + 0x30u, 0xaabbccddu);
    if (ram.read32(0x20u) != 0xabcdef12u ||
        ee::ee_mem_read32(&runtime, k_vu0_data + 0x30u) != 0xaabbccddu ||
        ee::ee_mem_read32(&runtime, k_vu0_code + 0x30u) != 0u ||
        ee::ee_mem_read32(&runtime, 0x11001000u) != 0u ||
        ee::ee_mem_read32(&runtime, 0x11003000u) != 0u ||
        ee::ee_mem_read32(&runtime, 0x11005000u) != 0u)
    {
        return fail("unmapped windows or main RAM isolation");
    }

    // The VU0 interpreter must fetch the same bytes written by EE memory
    // operations, not an unrelated shadow buffer.
    constexpr uint32_t code_offset = 0x80u;
    ee::ee_mem_write64(&runtime, k_vu0_code + code_offset,
                       0x4000000003e00008ull);
    vu0.m_micro_pc = code_offset;
    ee::s_ee_vu0_micro_instruction instruction{};
    if (ee::ee_vu0_fetch_micro_instruction(&vu0, &instruction) !=
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) ||
        instruction.m_pc != code_offset ||
        instruction.m_lower != 0x03e00008u ||
        instruction.m_upper != 0x40000000u ||
        vu0.m_micro_pc != code_offset + 8u)
    {
        return fail("VU microinstruction fetch did not see EE writes");
    }

    // Address dispatch is conditional on a real VU0 state; it must not
    // produce fake readable memory when a host hasn't installed a VU0.
    runtime.m_vu0_state = nullptr;
    ee::ee_mem_write32(&runtime, k_vu0_data + 4u, 0xffffffffu);
    if (ee::ee_mem_read32(&runtime, k_vu0_data + 4u) != 0 ||
        vu0.m_data_memory[4] != 0xefu)
    {
        return fail("mapping unexpectedly survives an absent VU state");
    }

    runtime.m_vu0_state = &vu0;
    ram.set_kseg_ram_aliases(true);
    if (ee::ee_mem_read32(&runtime, k_vu0_data + 4u) != 0xdeadbeefu ||
        ee::ee_mem_read32(&runtime, 0xa0000020u) != 0xabcdef12u)
    {
        return fail("VU0 data window interfered with KSEG RAM aliases");
    }

    std::puts("EE VU0 micro/data apertures: PASS");
    return 0;
}
