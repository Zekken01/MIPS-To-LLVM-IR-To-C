#pragma once

// Standalone native WebGPU input ABI, extracted from
// twist84/ps2-web-recomp:SDBZ/runtime/src/gs/gs.hpp.
// The original code is MIT licensed (see LICENSE.upstream).
// No browser, Emscripten, PS2 CPU, IOP, or game-runtime dependency.

#include <cstdint>
#include <vector>

namespace ps2::gs
{
inline constexpr std::uint32_t kMemorySize = 4u << 20;

enum Psm : std::uint32_t
{
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0a,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1b,
    PSMT4HL = 0x24, PSMT4HH = 0x2c,
    PSMZ32 = 0x30, PSMZ24 = 0x31,
    PSMZ16 = 0x32, PSMZ16S = 0x3a
};

struct Vertex
{
    std::uint32_t x{}, y{}, z{};
    std::uint8_t r{}, g{}, b{}, a{};
    float q{}, s{}, t{};
    std::uint32_t u{}, v{};
    std::uint8_t f{};
};

struct Context
{
    std::uint64_t tex0{}, clamp{}, tex1{}, tex2{}, xyoffset{}, miptbp1{},
        miptbp2{}, scissor{}, alpha{}, test{}, fba{}, frame{}, zbuf{};
};

struct State
{
    std::uint64_t prim{}, prmode{}, prmodecont{}, rgbaq{}, st{}, uv{},
        fog{}, texclut{}, scanmsk{}, texa{}, fogcol{}, dimx{}, dthe{},
        colclamp{}, pabe{};
    std::uint64_t bitbltbuf{}, trxpos{}, trxreg{}, trxdir{};
    Context ctx[2]{};
    float q = 1.0f;
    Vertex queue[3]{};
    int count{};
    std::uint32_t clut[512]{};
};

// This is the renderer's setup-cache generation, not guest memory state.
extern std::uint32_t g_setup_generation;

// Byte and nibble GS swizzle LUTs, generated from upstream's original
// GS tables; called when the native GPU initializes.
void swizzle_tables(std::vector<std::uint32_t>& out);
}
