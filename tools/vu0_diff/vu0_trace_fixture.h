#pragma once

#include <array>
#include <cstdint>

namespace vu0_trace_fixture
{

constexpr uint32_t k_micro_memory_size = 0x1000u;
constexpr uint32_t k_upper_nop = 0x000002ffu;
constexpr uint32_t k_idle_lower = 0x10000000u; // IADDIU VI0, VI0, 0
constexpr uint32_t k_step_limit = 64u;
constexpr uint32_t k_expected_step_count = 30u;
constexpr char k_program_sha256[] =
    "27cd26fb9a1a1e05aa1d9b6986904e2b860d6afd8a2688ec4e2d5270da8fe3a8";

struct s_pair
{
    uint32_t m_lower;
    uint32_t m_upper;
};

constexpr uint32_t make_iaddiu(uint32_t it, uint32_t is, uint32_t immediate)
{
    return (0x08u << 25) | (it << 16) | (is << 11) |
           ((immediate & 0x7800u) << 10) | (immediate & 0x07ffu);
}

constexpr uint32_t make_isubiu(uint32_t it, uint32_t is, uint32_t immediate)
{
    return (0x09u << 25) | (it << 16) | (is << 11) |
           ((immediate & 0x7800u) << 10) | (immediate & 0x07ffu);
}

constexpr uint32_t make_iaddi(uint32_t it, uint32_t is, int32_t immediate)
{
    return (0x3cu << 25) | (it << 16) | (is << 11) |
           ((static_cast<uint32_t>(immediate) & 0x1fu) << 6) | 0x32u;
}

constexpr uint32_t make_ialu_3r(
    uint32_t secondary,
    uint32_t it,
    uint32_t is,
    uint32_t id
)
{
    return (0x3cu << 25) | (it << 16) | (is << 11) |
           (id << 6) | secondary;
}

constexpr uint32_t make_branch(
    uint32_t primary,
    uint32_t it,
    uint32_t is,
    uint32_t immediate
)
{
    return (primary << 25) | (it << 16) | (is << 11) |
           (immediate & 0x07ffu);
}

// One deterministic 37-pair fixture. It executes all seven supported lower
// scalar integer operations, B/BAL/JR/JALR, all six conditional branches,
// an M-marked pair, and an E-marked pair with its delay pair. Unreachable
// slots are filled with a harmless IADDIU VI0, VI0, 0.
constexpr std::array<s_pair, 37> k_program =
{{
    { make_iaddiu(1, 0, 5), k_upper_nop },                         //  0
    { make_iaddi(2, 1, -1), k_upper_nop },                         //  1
    { make_ialu_3r(0x30, 1, 2, 3), k_upper_nop },                  //  2 IADD
    { make_ialu_3r(0x31, 1, 2, 4), k_upper_nop },                  //  3 ISUB
    { make_ialu_3r(0x34, 1, 3, 5), k_upper_nop | 0x20000000u },   //  4 IAND + M
    { make_ialu_3r(0x35, 1, 2, 6), k_upper_nop },                  //  5 IOR
    { make_isubiu(7, 6, 1), k_upper_nop },                         //  6
    { make_branch(0x20, 0, 0, 2), k_upper_nop },                   //  7 B -> 10
    { make_iaddiu(8, 0, 8), k_upper_nop },                         //  8 delay
    { make_iaddiu(9, 0, 99), k_upper_nop },                        //  9 skipped
    { make_branch(0x21, 10, 0, 2), k_upper_nop },                  // 10 BAL -> 13
    { make_iaddiu(8, 0, 11), k_upper_nop },                        // 11 delay
    { make_iaddiu(12, 0, 12), k_upper_nop },                       // 12 skipped
    { make_iaddiu(13, 0, 16), k_upper_nop },                       // 13
    { make_branch(0x24, 0, 13, 0), k_upper_nop },                  // 14 JR -> 16
    { make_iaddiu(15, 0, 21), k_upper_nop },                       // 15 delay
    { make_branch(0x25, 15, 15, 0), k_upper_nop },                 // 16 JALR VI15 -> 21, link VI15
    { make_iaddiu(8, 0, 17), k_upper_nop },                        // 17 delay
    { { k_idle_lower }, k_upper_nop },                             // 18 skipped
    { { k_idle_lower }, k_upper_nop },                             // 19 skipped
    { { k_idle_lower }, k_upper_nop },                             // 20 skipped
    { make_branch(0x28, 1, 1, 2), k_upper_nop },                   // 21 IBEQ true -> 24
    { make_iaddiu(8, 0, 22), k_upper_nop },                        // 22 delay
    { make_iaddiu(9, 0, 23), k_upper_nop },                        // 23 skipped
    { make_branch(0x29, 1, 1, 2), k_upper_nop },                   // 24 IBNE false
    { make_iaddiu(8, 0, 25), k_upper_nop },                        // 25 sequential/delay
    { make_branch(0x2c, 0, 1, 2), k_upper_nop },                   // 26 IBLTZ false
    { make_iaddiu(8, 0, 27), k_upper_nop },                        // 27 sequential/delay
    { make_branch(0x2d, 0, 1, 1), k_upper_nop },                   // 28 IBGTZ true -> 30
    { make_iaddiu(8, 0, 29), k_upper_nop },                        // 29 delay
    { make_branch(0x2e, 0, 1, 2), k_upper_nop },                   // 30 IBLEZ false
    { make_iaddiu(8, 0, 31), k_upper_nop },                        // 31 sequential/delay
    { make_branch(0x2f, 0, 1, 2), k_upper_nop },                   // 32 IBGEZ true -> 35
    { make_iaddiu(8, 0, 33), k_upper_nop },                        // 33 delay
    { { k_idle_lower }, k_upper_nop },                             // 34 skipped
    { make_iaddiu(3, 0, 0x333), k_upper_nop | 0x40000000u },       // 35 E
    { make_iaddiu(8, 0, 36), k_upper_nop }                         // 36 E delay
}};

static_assert(k_program.size() == 37);

inline void install_program(uint8_t* micro_memory, uint32_t memory_size)
{
    for (uint32_t i = 0; i < memory_size; ++i)
        micro_memory[i] = 0;

    for (uint32_t i = 0; i < k_program.size(); ++i)
    {
        const s_pair& pair = k_program[i];
        const uint32_t offset = i * 8u;
        const uint32_t words[2] = { pair.m_lower, pair.m_upper };
        for (uint32_t word = 0; word < 2; ++word)
        {
            const uint32_t value = words[word];
            const uint32_t byte_offset = offset + word * 4u;
            micro_memory[byte_offset + 0] = static_cast<uint8_t>(value);
            micro_memory[byte_offset + 1] = static_cast<uint8_t>(value >> 8);
            micro_memory[byte_offset + 2] = static_cast<uint8_t>(value >> 16);
            micro_memory[byte_offset + 3] = static_cast<uint8_t>(value >> 24);
        }
    }
}

} // namespace vu0_trace_fixture
