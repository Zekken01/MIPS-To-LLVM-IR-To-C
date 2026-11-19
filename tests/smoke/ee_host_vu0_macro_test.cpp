#include "../../src/tools/ee_host_vu0_macro.h"

#include <cstdint>
#include <cstdio>

namespace
{
uint32_t s_sync_result = 0u;
uint32_t s_sync_calls = 0u;

uint32_t vu0_sync(void*, ee::s_ee_cpu_state*,
                  ee::s_ee_vu0_state*, uint32_t mode, uint32_t)
{
    ++s_sync_calls;
    if (mode != static_cast<uint32_t>(
            ee::e_ee_vu0_sync_mode::FINISH_TO_E))
    {
        return static_cast<uint32_t>(
            ee::e_ee_vu0_sync_result::FAILED);
    }
    return s_sync_result;
}

uint32_t make_opcode(uint32_t secondary, uint32_t mask,
                     uint32_t fs, uint32_t ft)
{
    return 0x4a000000u | (mask << 21u) | (ft << 16u) |
        (fs << 11u) | ((secondary >> 2u) << 6u) |
        (0x3cu | (secondary & 3u));
}

int check(bool valid, const char* message)
{
    if (!valid)
    {
        std::fprintf(stderr, "VU0 indexed macro: %s\n", message);
        return 1;
    }
    return 0;
}

uint32_t lane(const ee::s_ee_u128& vector, uint32_t index)
{
    const uint64_t value = index < 2u ? vector.m_lo : vector.m_hi;
    return static_cast<uint32_t>(value >> ((index & 1u) * 32u));
}
}

int main()
{
    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::s_ee_runtime rt{};
    rt.m_vu0_state = &vu0;
    rt.m_vu0_sync = &vu0_sync;

    const ee::s_ee_u128 vector{
        0x89abcdef01234567ull, 0xfedcba9876543210ull
    };
    cpu.m_vu0[3] = vector;
    ee::ee_vu0_write_vi(&vu0, 1u, 0xffu);
    const uint32_t sqi = make_opcode(0x35u, 0xfu, 3u, 1u);
    const uint32_t lqi = make_opcode(0x34u, 0xfu, 1u, 2u);
    const uint32_t lqd = make_opcode(0x36u, 0xfu, 1u, 2u);
    const uint32_t sqd = make_opcode(0x37u, 0xfu, 3u, 1u);
    if (check(ee::c_ee_host_vu0_macro::indexed_memory_opcode(
            0x4be10b7cu), "real SDBZ VLQI opcode was not recognized") ||
        check(!ee::c_ee_host_vu0_macro::indexed_memory_opcode(
            0x4be10b3cu), "unrelated SPECIAL2 operation accepted"))
    {
        return 1;
    }

    // Last vector in VU0's 4 KiB data memory. VSQI must write all four
    // lanes and then increment VI1, wrapping the 8-bit quadword index.
    if (check(ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, sqi, 0x00100008u), "VSQI rejected") ||
        check(ee::ee_vu0_read_vi(&vu0, 1u) == 0x100u,
              "postincrement changed wrong VI value"))
    {
        return 1;
    }
    ee::ee_vu0_write_vi(&vu0, 1u, 0x1ffu);
    if (check(ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, lqi, 0x0010000cu), "VLQI rejected") ||
        check(ee::ee_vu0_read_vi(&vu0, 1u) == 0x200u,
              "postincrement failed at aliased memory index") ||
        check(cpu.m_vu0[2].m_lo == vector.m_lo &&
              cpu.m_vu0[2].m_hi == vector.m_hi,
              "VLQI did not read VU0 data memory"))
    {
        return 1;
    }

    // Masked loads preserve unselected VF lanes, with no VF0 writes.
    ee::ee_vu0_write_vi(&vu0, 1u, 0xffu);
    cpu.m_vu0[2] = {0xaaaaaaaabbbbbbbbull, 0xccccccccddddddddull};
    if (check(ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, make_opcode(0x34u, 0xau, 1u, 2u),
            0x00100010u), "masked VLQI rejected") ||
        check(lane(cpu.m_vu0[2], 0u) == lane(vector, 0u) &&
              lane(cpu.m_vu0[2], 1u) == 0xaaaaaaaau &&
              lane(cpu.m_vu0[2], 2u) == lane(vector, 2u) &&
              lane(cpu.m_vu0[2], 3u) == 0xccccccccu,
              "masked VLQI overwrote unselected lanes"))
    {
        return 1;
    }

    ee::ee_vu0_write_vi(&vu0, 1u, 0x100u);
    cpu.m_vu0[2] = {};
    if (check(ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, lqd, 0x00100014u), "VLQD rejected") ||
        check(ee::ee_vu0_read_vi(&vu0, 1u) == 0xffu,
              "VLQD did not predecrement") ||
        check(cpu.m_vu0[2].m_lo == vector.m_lo &&
              cpu.m_vu0[2].m_hi == vector.m_hi,
              "VLQD loaded incorrect vector"))
    {
        return 1;
    }

    // Store predecrement and VF0 constant register handling.
    ee::ee_vu0_write_vi(&vu0, 1u, 0x100u);
    cpu.m_vu0[3] = {0x5555555544444444ull, 0x7777777766666666ull};
    if (check(ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, sqd, 0x00100018u), "VSQD rejected") ||
        check(ee::ee_vu0_read_vi(&vu0, 1u) == 0xffu,
              "VSQD did not predecrement"))
    {
        return 1;
    }
    const ee::s_ee_u128 saved_vf0 = cpu.m_vu0[0];
    if (check(ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, make_opcode(0x34u, 0xfu, 1u, 0u),
            0x0010001cu), "VLQI VF0 rejected") ||
        check(cpu.m_vu0[0].m_lo == saved_vf0.m_lo &&
              cpu.m_vu0[0].m_hi == saved_vf0.m_hi,
              "hardwired VF0 was modified"))
    {
        return 1;
    }

    // Fail-closed: invalid index and failed VU0 synchronization cannot
    // write either VF or the VI address register.
    const uint32_t prior_vi = ee::ee_vu0_read_vi(&vu0, 1u);
    const uint32_t prior_calls = s_sync_calls;
    if (check(!ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, make_opcode(0x34u, 0xfu, 17u, 2u),
            0x00100020u), "control register accepted as memory base") ||
        check(s_sync_calls == prior_calls,
              "invalid base invoked VU0 synchronization"))
    {
        return 1;
    }
    s_sync_result = static_cast<uint32_t>(
        ee::e_ee_vu0_sync_result::UNAVAILABLE);
    const ee::s_ee_u128 before = cpu.m_vu0[2];
    if (check(!ee::c_ee_host_vu0_macro::execute(
            &cpu, &rt, lqi, 0x00100024u),
              "failed VU0 sync silently succeeded") ||
        check(prior_vi == ee::ee_vu0_read_vi(&vu0, 1u),
              "failed VU0 sync altered VI") ||
        check(before.m_lo == cpu.m_vu0[2].m_lo &&
              before.m_hi == cpu.m_vu0[2].m_hi,
              "failed VU0 sync altered VF"))
    {
        return 1;
    }

    std::puts("host VU0 indexed macro semantics pass");
    return 0;
}
