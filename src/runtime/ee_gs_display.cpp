#include "ee_gs_display.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ee
{

bool c_ee_gs_display::readback(
    const c_ee_gs_state& gs,
    const s_ee_gs_display_registers& registers,
    uint32_t crt_mode,
    std::vector<uint32_t>& pixels,
    uint32_t& width,
    uint32_t& height
)
{
    const uint32_t circuit =
        (registers.m_pmode & 2u) != 0u ? 1u : 0u;
    if ((registers.m_pmode & 3u) == 0u ||
        (crt_mode != 2u && crt_mode != 3u))
    {
        return false;
    }

    const uint64_t fb = registers.m_dispfb[circuit];
    const uint64_t disp = registers.m_display[circuit];
    const uint32_t base = (static_cast<uint32_t>(fb) & 0x1ffu) * 32u;
    const uint32_t fbw = static_cast<uint32_t>((fb >> 9u) & 0x3fu);
    const uint32_t psm = static_cast<uint32_t>((fb >> 15u) & 0x1fu);
    const uint32_t dbx = static_cast<uint32_t>((fb >> 32u) & 0x7ffu);
    const uint32_t dby = static_cast<uint32_t>((fb >> 43u) & 0x7ffu);
    const uint32_t magh = static_cast<uint32_t>((disp >> 23u) & 0xfu) + 1u;
    const uint32_t magv = static_cast<uint32_t>((disp >> 27u) & 3u) + 1u;
    const uint32_t dw = static_cast<uint32_t>((disp >> 32u) & 0xfffu) + 1u;
    const uint32_t raw_dh = static_cast<uint32_t>((disp >> 44u) & 0x7ffu) + 1u;
    const uint32_t dy = static_cast<uint32_t>((disp >> 12u) & 0x7ffu);
    const bool pal = crt_mode == 3u;
    const uint32_t scale = (registers.m_smode2 & 1u) != 0u ? 2u : 1u;
    const uint32_t lines = (pal ? 288u : 240u) * scale;
    const uint32_t top = (pal ? 36u : 25u) * scale;
    const uint32_t span = (pal ? 256u : 224u) * scale;
    const uint32_t below = dy > top ? std::min(dy - top, span) : 0u;
    const uint32_t dh = std::min(raw_dh,
        std::min(lines, below != 0u ? span - below : lines));
    const uint32_t w = dw / magh;
    const uint32_t h = dh / magv;
    if (fbw == 0u || w == 0u || h == 0u || w > 2048u || h > 2048u)
    {
        return false;
    }

    const bool half_source_y = (registers.m_smode2 & 3u) == 3u;
    std::vector<uint32_t> next(static_cast<size_t>(w) * h);
    for (uint32_t y = 0u; y < h; ++y)
    {
        for (uint32_t x = 0u; x < w; ++x)
        {
            const uint32_t sx = dbx + x;
            const uint32_t sy = dby + (half_source_y ? y / 2u : y);
            uint32_t rgba = 0u;
            if (sx >= 2048u || sy >= 2048u ||
                !gs.read_color_pixel(psm, base, fbw, sx, sy, rgba))
            {
                return false;
            }
            next[static_cast<size_t>(y) * w + x] = rgba;
        }
    }
    pixels.swap(next);
    width = w;
    height = h;
    return true;
}

bool c_ee_gs_display::readback_upload(
    const c_ee_gs_state& gs, std::vector<uint32_t>& pixels,
    uint32_t& width, uint32_t& height
)
{
    const s_ee_gs_state& state = gs.state();
    if (!state.m_transfer_started ||
        !state.m_written[0x50u] || !state.m_written[0x51u] ||
        !state.m_written[0x52u] || !state.m_written[0x53u] ||
        (state.m_registers[0x53u] & 3u) != 0u)
    {
        return false;
    }

    const uint64_t buffer = state.m_registers[0x50u];
    const uint64_t position = state.m_registers[0x51u];
    const uint64_t size = state.m_registers[0x52u];
    const uint32_t base = static_cast<uint32_t>((buffer >> 32u) & 0x3fffu);
    const uint32_t buffer_width = static_cast<uint32_t>(
        (buffer >> 48u) & 0x3fu);
    const uint32_t psm = static_cast<uint32_t>((buffer >> 56u) & 0x3fu);
    const uint32_t dx = static_cast<uint32_t>((position >> 32u) & 0x7ffu);
    const uint32_t dy = static_cast<uint32_t>((position >> 48u) & 0x7ffu);
    const uint32_t w = static_cast<uint32_t>(size & 0xfffu);
    const uint32_t h = static_cast<uint32_t>((size >> 32u) & 0xfffu);

    // A partial upload must never be shown as a complete texture.
    if (w == 0u || h == 0u || w > 2048u || h > 2048u ||
        buffer_width == 0u || dx + w > 2048u || dy + h > 2048u ||
        static_cast<uint64_t>(w) * h != state.m_transfer_pixels_written)
    {
        return false;
    }

    std::vector<uint32_t> next(static_cast<size_t>(w) * h);
    for (uint32_t y = 0u; y < h; ++y)
    {
        for (uint32_t x = 0u; x < w; ++x)
        {
            uint32_t color = 0u;
            if (!gs.read_color_pixel(
                    psm, base, buffer_width, dx + x, dy + y, color))
            {
                return false;
            }
            next[static_cast<size_t>(y) * w + x] = color;
        }
    }
    pixels.swap(next);
    width = w;
    height = h;
    return true;
}

} // namespace ee
