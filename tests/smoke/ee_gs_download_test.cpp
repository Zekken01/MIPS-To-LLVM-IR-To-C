#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS download: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t bw, uint32_t psm, bool source)
{
    return source ?
        static_cast<uint64_t>(bp) |
        (static_cast<uint64_t>(bw) << 16u) |
        (static_cast<uint64_t>(psm) << 24u) :
        (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(bw) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxpos(uint32_t x, uint32_t y, bool source)
{
    return source ?
        static_cast<uint64_t>(x) |
        (static_cast<uint64_t>(y) << 16u) :
        (static_cast<uint64_t>(x) << 32u) |
        (static_cast<uint64_t>(y) << 48u);
}

uint64_t trxreg(uint32_t w, uint32_t h)
{
    return w | (static_cast<uint64_t>(h) << 32u);
}

bool reg(ee::c_ee_gs_state& gs, uint32_t index, uint64_t value)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::packed;
    event.m_register = 0xeu;
    event.m_word1 = index;
    event.m_word0 = value;
    const std::array<uint8_t, 16> dummy{};
    return gs.consume(&event, 1u, dummy.data(), 1u);
}

bool image(ee::c_ee_gs_state& gs, const uint8_t* data, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, data, qwc);
}

bool setup(ee::c_ee_gs_state& gs, uint32_t bp, uint32_t bw,
           uint32_t psm, uint32_t x, uint32_t y,
           uint32_t w, uint32_t h, bool download)
{
    return reg(gs, 0x50u, bitblt(bp, bw, psm, download)) &&
        reg(gs, 0x51u, trxpos(x, y, download)) &&
        reg(gs, 0x52u, trxreg(w, h)) &&
        reg(gs, 0x53u, download ? 1u : 0u);
}

bool upload(ee::c_ee_gs_state& gs, uint32_t bp, uint32_t bw,
            uint32_t psm, uint32_t x, uint32_t y,
            uint32_t w, uint32_t h,
            const uint8_t* data, uint32_t qwc)
{
    return setup(gs, bp, bw, psm, x, y, w, h, false) &&
        image(gs, data, qwc);
}

void set32(uint8_t* data, uint32_t i, uint32_t value)
{
    for (uint32_t b = 0u; b < 4u; ++b)
    {
        data[i * 4u + b] = static_cast<uint8_t>(value >> (b * 8u));
    }
}

bool test_one_pixel_formats()
{
    ee::c_ee_gs_state gs;
    struct s_format
    {
        uint32_t psm;
        uint32_t bits;
    };
    const s_format formats[] =
    {
        {0x00u, 32u}, {0x01u, 24u}, {0x02u, 16u},
        {0x0au, 16u}, {0x13u, 8u}, {0x14u, 4u},
        {0x1bu, 8u}, {0x24u, 4u}, {0x2cu, 4u},
        {0x30u, 32u}, {0x31u, 24u}, {0x32u, 16u},
        {0x3au, 16u}
    };
    std::array<uint8_t, 16> upload_data{};
    set32(upload_data.data(), 0u, 0x12345678u);

    for (const s_format& f : formats)
    {
        gs.reset();
        if (!upload(gs, 0x100u, 1u, f.psm, 0u, 0u,
                    1u, 1u, upload_data.data(), 1u) ||
            !setup(gs, 0x100u, 1u, f.psm, 0u, 0u,
                   1u, 1u, true) ||
            !gs.state().m_download_active ||
            gs.state().m_download_bytes_read != 0u ||
            gs.state().m_download_qwc != 0u ||
            gs.state().m_transfer_started)
        {
            return false;
        }
        std::array<uint8_t, 16> result{};
        result.fill(0xeeu);
        if (!gs.read_image(result.data(), 1u))
        {
            return false;
        }
        const uint32_t logical_bytes = (f.bits + 7u) / 8u;
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            const uint8_t expected = i < logical_bytes ?
                (f.bits == 4u ? 0x08u : upload_data[i]) : 0u;
            if (result[i] != expected)
            {
                return false;
            }
        }
        if (gs.state().m_download_active ||
            gs.state().m_download_bytes_read != 16u ||
            gs.state().m_download_qwc != 1u ||
            gs.state().m_image_qwc != 1u)
        {
            return false;
        }

        // Exhausted reads yield zero bytes; no stale pixel data.
        result.fill(0xeeu);
        if (!gs.read_image(result.data(), 1u) ||
            gs.state().m_download_qwc != 2u)
        {
            return false;
        }
        for (uint8_t byte : result)
        {
            if (byte != 0u)
            {
                return false;
            }
        }
    }
    return true;
}

bool test_24_bit_carry()
{
    ee::c_ee_gs_state gs;
    for (const uint32_t psm : {0x01u, 0x31u})
    {
        gs.reset();
        std::array<uint8_t, 32> in{};
        in.fill(0xcdu);
        for (uint32_t i = 0u; i < 21u; ++i)
        {
            in[i] = static_cast<uint8_t>(0x20u + i);
        }
        if (!upload(gs, 0x100u, 2u, psm, 0u, 31u,
                    7u, 1u, in.data(), 2u) ||
            !setup(gs, 0x100u, 2u, psm, 0u, 31u,
                   7u, 1u, true))
        {
            return false;
        }

        std::array<uint8_t, 16> first{};
        std::array<uint8_t, 16> second{};
        first.fill(0xeeu);
        second.fill(0xeeu);
        if (!gs.read_image(first.data(), 1u) ||
            !gs.state().m_download_active ||
            gs.state().m_download_bytes_read != 16u ||
            !gs.read_image(second.data(), 1u) ||
            gs.state().m_download_active ||
            gs.state().m_download_bytes_read != 32u ||
            gs.state().m_download_qwc != 2u)
        {
            return false;
        }
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            if (first[i] != in[i] ||
                second[i] != (i < 5u ? in[16u + i] : 0u))
            {
                return false;
            }
        }
    }
    return true;
}

bool test_nibble_across_rows()
{
    ee::c_ee_gs_state gs;
    // 7x5 is odd-width and 35 pixels, crossing the 16-byte image
    // boundary. The final source nibble must not leak into padding.
    for (const uint32_t psm : {0x14u, 0x24u, 0x2cu})
    {
        gs.reset();
        std::array<uint8_t, 32> in{};
        in.fill(0xf0u);
        for (uint32_t i = 0u; i < 35u; ++i)
        {
            const uint8_t value = static_cast<uint8_t>(
                (i * 7u + 1u) & 15u
            );
            const uint32_t at = i / 2u;
            const uint8_t shift = static_cast<uint8_t>(
                (i & 1u) * 4u
            );
            const uint8_t mask = static_cast<uint8_t>(15u << shift);
            in[at] = static_cast<uint8_t>(
                (in[at] & static_cast<uint8_t>(~mask)) |
                (value << shift)
            );
        }
        if (!upload(gs, 0x100u, 2u, psm, 0u, 0u,
                    7u, 5u, in.data(), 2u) ||
            !setup(gs, 0x100u, 2u, psm, 0u, 0u,
                   7u, 5u, true))
        {
            return false;
        }
        std::array<uint8_t, 16> first{};
        std::array<uint8_t, 16> second{};
        if (!gs.read_image(first.data(), 1u) ||
            !gs.state().m_download_active ||
            !gs.read_image(second.data(), 1u) ||
            gs.state().m_download_active)
        {
            return false;
        }
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            if (first[i] != in[i])
            {
                return false;
            }
            const uint8_t expected = i == 0u ? in[16u] :
                i == 1u ? static_cast<uint8_t>(in[17u] & 0x0fu) :
                0u;
            if (second[i] != expected)
            {
                return false;
            }
        }
    }
    return true;
}

bool test_boundary_and_invalid()
{
    ee::c_ee_gs_state gs;
    std::array<uint8_t, 16> result{};
    result.fill(0x5au);
    if (gs.read_image(result.data(), 1u) ||
        gs.read_image(nullptr, 1u) ||
        gs.read_image(result.data(), 0u) ||
        ee::c_ee_gs_state::dma_download(&gs, 2u, result.data(), 1u) ||
        ee::c_ee_gs_state::dma_download(nullptr, 1u, result.data(), 1u) ||
        reg(gs, 0x53u, 1u) ||
        gs.was_written(0x53u) ||
        gs.state().m_download_bytes_read != 0u)
    {
        return false;
    }

    // Invalid source width or format rejects the register transaction.
    if (!reg(gs, 0x50u, bitblt(0x100u, 0u, 0u, true)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u, true)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        reg(gs, 0x53u, 1u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 1u, 0x03u, true)) ||
        reg(gs, 0x53u, 1u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 1u, 0u, true)) ||
        !reg(gs, 0x52u, trxreg(0u, 1u)) ||
        reg(gs, 0x53u, 1u) ||
        gs.was_written(0x53u))
    {
        return false;
    }

    // Cross X=2047 -> 0 and Y=31 into Y=32; local address calculation
    // must be the same swizzled path used by uploads.
    gs.reset();
    std::array<uint8_t, 16> pixels{};
    set32(pixels.data(), 0u, 0x13572468u);
    set32(pixels.data(), 1u, 0x89abcdefu);
    if (!upload(gs, 0x300u, 2u, 0u, 2047u, 31u,
                2u, 1u, pixels.data(), 1u) ||
        !setup(gs, 0x300u, 2u, 0u, 2047u, 31u,
                   2u, 1u, true) ||
        !ee::c_ee_gs_state::dma_download(
            &gs, 1u, result.data(), 1u))
    {
        return false;
    }
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        if (result[i] != (i < 8u ? pixels[i] : 0u))
        {
            return false;
        }
    }

    // Snapshot of the transfer state is restarted by TRXDIR=1.
    if (!reg(gs, 0x53u, 1u) ||
        !gs.state().m_download_active ||
        gs.state().m_download_bytes_read != 0u ||
        !gs.read_image(result.data(), 1u) ||
        gs.state().m_download_active)
    {
        return false;
    }
    gs.reset();
    if (gs.state().m_download_active ||
        gs.state().m_download_bytes_read != 0u ||
        gs.state().m_download_qwc != 0u)
    {
        return false;
    }
    return true;
}
} // namespace

int main()
{
    if (!test_one_pixel_formats())
    {
        return fail("13-format one-pixel readback and padding");
    }
    if (!test_24_bit_carry())
    {
        return fail("PSMCT24/PSMZ24 split-qword carry");
    }
    if (!test_nibble_across_rows())
    {
        return fail("PSMT4/HL/HH nibble ordering across odd rows");
    }
    if (!test_boundary_and_invalid())
    {
        return fail("source setup, coordinate wrap or download reset");
    }
    std::puts("GS TRXDIR=1 unswizzle, 13 PSMs, partial QWC: PASS");
    return 0;
}
