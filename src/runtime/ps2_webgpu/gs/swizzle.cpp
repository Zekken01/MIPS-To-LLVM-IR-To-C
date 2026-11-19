// MIT licensed source adapted from twist84/ps2-web-recomp:SDBZ/runtime/src/gs/gs.cpp
// Unmodified GS swizzle tables; standalone initialization only.
#include "gs.hpp"
#include <iterator>

namespace ps2::gs
{
uint32_t g_setup_generation = 0u;

namespace
{
std::uint32_t g_off32[32][64], g_offz32[32][64];
std::uint32_t g_off16[64][64], g_off16s[64][64], g_offz16[64][64], g_offz16s[64][64];
std::uint32_t g_off8[64][128], g_off4[128][128];

const int kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const int kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
const int kColumn32[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
const int kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const int kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
const int kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
const int kBlockZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
const int kColumn16[2][16] = {{0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};
const int kColumnWord[2][2][8] = {{{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}}, {{8, 9, 12, 13, 0, 1, 4, 5}, {10, 11, 14, 15, 2, 3, 6, 7}}};

void build_tables() {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 7;
            g_off32[y][x] = static_cast<std::uint32_t>(kBlock32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
            g_offz32[y][x] = static_cast<std::uint32_t>(kBlockZ32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 15;
            const int inner = column * 64 + kColumn16[cy][cx] * 2;
            g_off16[y][x] = static_cast<std::uint32_t>(kBlock16[y / 8][x / 16] * 256 + inner);
            g_off16s[y][x] = static_cast<std::uint32_t>(kBlock16S[y / 8][x / 16] * 256 + inner);
            g_offz16[y][x] = static_cast<std::uint32_t>(kBlockZ16[y / 8][x / 16] * 256 + inner);
            g_offz16s[y][x] = static_cast<std::uint32_t>(kBlockZ16S[y / 8][x / 16] * 256 + inner);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock32[(y / 16) & 3][(x / 16) & 7];
            int wx = x % 16, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            const std::uint32_t byte = static_cast<std::uint32_t>(((wx & 8) >> 2) + ((wy & 2) >> 1));
            table ^= static_cast<std::uint32_t>((y / 4) & 1);
            g_off8[y][x] = static_cast<std::uint32_t>(block * 256 + column * 64 + kColumnWord[table][wy & 1][wx & 7] * 4) + byte;
        }
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock16[(y / 16) & 7][(x / 32) & 3];
            int wx = x % 32, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            const std::uint32_t shift = static_cast<std::uint32_t>((wx & 0x18) + ((wy & 2) << 1));
            const std::uint32_t nibble = shift / 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            table ^= static_cast<std::uint32_t>(column & 1);
            g_off4[y][x] = static_cast<std::uint32_t>((column * 64 + block * 256 + kColumnWord[table][wy & 1][wx & 7] * 4) * 2) + nibble;
        }
}


}
void swizzle_tables(std::vector<std::uint32_t> &out) {
    static const bool initialized = []() { build_tables(); return true; }();
    (void)initialized;
    out.clear();
    for (auto &row : g_off32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off8) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off4) out.insert(out.end(), std::begin(row), std::end(row));
}


} // namespace ps2::gs
