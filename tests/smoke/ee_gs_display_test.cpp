#include "ee_gs_display.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int g_failed = 0;

void check(bool success, const char* label)
{
    if (!success)
    {
        std::printf("FAIL: %s\n", label);
        ++g_failed;
    }
}

bool upload(ee::c_ee_gs_state& gs, uint32_t psm,
            const std::vector<uint8_t>& source)
{
    if (source.size() != 32u)
    {
        return false;
    }
    uint8_t data[96]{};
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        data[64u + i] = source[i];
    }
    ee::s_ee_gif_event events[5]{};
    const uint32_t regs[4] = {0x50u, 0x51u, 0x52u, 0x53u};
    const uint64_t values[4] =
    {
        (uint64_t{1} << 48u) | (uint64_t{psm} << 56u),
        0u,
        uint64_t{8} | (uint64_t{1} << 32u),
        0u
    };
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        events[i].m_type = ee::e_ee_gif_event_type::packed;
        events[i].m_register = 0x0eu;
        events[i].m_byte_offset = i * 16u;
        events[i].m_word0 = values[i];
        events[i].m_word1 = regs[i];
    }
    events[4].m_type = ee::e_ee_gif_event_type::image;
    events[4].m_byte_offset = 64u;
    events[4].m_qwc = 2u;
    return gs.consume(events, 5u, data, 6u);
}
}

int main()
{
    ee::c_ee_gs_state gs{};
    ee::s_ee_gs_display_registers regs{};
    std::vector<uint32_t> pixels{0x12345678u};
    uint32_t width = 99u;
    uint32_t height = 88u;
    check(!ee::c_ee_gs_display::readback(
              gs, regs, 2u, pixels, width, height) &&
          pixels.size() == 1u && width == 99u && height == 88u,
          "disabled PMODE leaves outputs untouched");

    regs.m_pmode = 1u;
    regs.m_dispfb[0] = uint64_t{1} << 9u; // FBW=1
    regs.m_display[0] = uint64_t{7} << 32u; // width=8 height=1
    std::vector<uint8_t> source(32u);
    for (uint32_t p = 0u; p < 8u; ++p)
    {
        source[p * 4u] = static_cast<uint8_t>(p + 1u);
        source[p * 4u + 1u] = static_cast<uint8_t>(p + 21u);
        source[p * 4u + 2u] = static_cast<uint8_t>(p + 41u);
    }
    check(upload(gs, 0u, source), "real GS IMAGE upload accepted");
    check(ee::c_ee_gs_display::readback(
              gs, regs, 2u, pixels, width, height) &&
          width == 8u && height == 1u && pixels.size() == 8u &&
          pixels[0] == 0xff291501u &&
          pixels[7] == 0xff301c08u,
          "PSMCT32 display readback follows swizzled IMAGE upload");

    regs.m_pmode = 2u;
    regs.m_dispfb[1] = regs.m_dispfb[0];
    regs.m_display[1] = regs.m_display[0];
    check(ee::c_ee_gs_display::readback(
              gs, regs, 2u, pixels, width, height) &&
          pixels[4] == 0xff2d1905u,
          "second GS display circuit uses its own guest registers");

    regs.m_dispfb[1] |= uint64_t{0x13u} << 15u;
    const std::vector<uint32_t> before = pixels;
    check(!ee::c_ee_gs_display::readback(
              gs, regs, 2u, pixels, width, height) &&
          pixels == before,
          "indexed-texture PSM refuses false display output");

    regs.m_pmode = 0u;
    check(!ee::c_ee_gs_display::readback(
              gs, regs, 2u, pixels, width, height),
          "no scanout is manufactured when display is disabled");

    std::printf("ee_gs_display_test: %d failure(s)\n", g_failed);
    return g_failed != 0 ? 1 : 0;
}
