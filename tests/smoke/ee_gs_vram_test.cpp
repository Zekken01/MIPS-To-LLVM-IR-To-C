#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS VRAM: %s\n", reason);
    return 1;
}

bool write_register(ee::c_ee_gs_state& gs, uint32_t reg, uint64_t value)
{
    // PACKED A+D forwards the register index in the high word.
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::packed;
    event.m_register = 0xeu;
    event.m_word0 = value;
    event.m_word1 = reg;
    const std::array<uint8_t, 16> packet{};
    return gs.consume(&event, 1u, packet.data(), 1u);
}

bool write_image(
    ee::c_ee_gs_state& gs, const uint8_t* data, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, data, qwc);
}

uint32_t read32(const ee::c_ee_gs_state& gs, uint32_t address)
{
    uint32_t value = 0u;
    if (!gs.read_vram32(address, value))
    {
        return 0xffffffffu;
    }
    return value;
}

uint64_t bitblt(uint32_t bp, uint32_t bw, uint32_t psm)
{
    return (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(bw) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxreg(uint32_t width, uint32_t height)
{
    return width | (static_cast<uint64_t>(height) << 32u);
}

void set_pixel(std::vector<uint8_t>& data, size_t index, uint32_t value)
{
    const size_t offset = index * 4u;
    data[offset + 0u] = static_cast<uint8_t>(value);
    data[offset + 1u] = static_cast<uint8_t>(value >> 8u);
    data[offset + 2u] = static_cast<uint8_t>(value >> 16u);
    data[offset + 3u] = static_cast<uint8_t>(value >> 24u);
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x20u;
    constexpr uint32_t k_base = k_bp * 256u;

    if (gs.vram().size() != ee::c_ee_gs_state::k_vram_bytes ||
        read32(gs, k_base) != 0u)
    {
        return fail("4 MiB GS VRAM not initialized independently");
    }

    // The transfer rejects absent/zero DBW and still-unsupported
    // pixel formats before accepting IMAGE payloads.
    if (!write_register(gs, 0x50u, bitblt(k_bp, 0u, 0u)) ||
        !write_register(gs, 0x51u, 0u) ||
        !write_register(gs, 0x52u, trxreg(1u, 1u)) ||
        write_register(gs, 0x53u, 0u) ||
        gs.was_written(0x53u) ||
        !write_register(gs, 0x50u, bitblt(k_bp, 1u, 0x13u)) ||
        write_register(gs, 0x53u, 0u) ||
        !write_register(gs, 0x50u, bitblt(k_bp, 1u, 0u)) ||
        !write_register(gs, 0x52u, trxreg(0u, 1u)) ||
        write_register(gs, 0x53u, 0u))
    {
        return fail("invalid DBW, PSMT8 or zero-size transfer was accepted");
    }

    // Two pages across the X axis, two rows, and two unused padding
    // pixels. The test checks independent, known-good GS byte offsets,
    // not a calculation shared with the implementation.
    constexpr uint32_t k_width = 65u;
    constexpr uint32_t k_height = 2u;
    constexpr uint32_t k_pixels = k_width * k_height;
    std::vector<uint8_t> image(33u * 16u, 0xccu);
    for (uint32_t i = 0u; i < k_pixels; ++i)
    {
        set_pixel(image, i, 0xa0000000u | (i + 1u));
    }

    if (!write_register(gs, 0x50u, bitblt(k_bp, 2u, 0u)) ||
        !write_register(gs, 0x52u, trxreg(k_width, k_height)) ||
        !write_register(gs, 0x53u, 0u) ||
        !gs.state().m_transfer_started ||
        !write_image(gs, image.data(), 16u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        read32(gs, k_base) != 0xa0000001u ||
        read32(gs, k_base + 256u) != 0xa0000009u ||
        read32(gs, k_base + 8192u) != 0u)
    {
        return fail("first DMA image chunk or PSMCT32 block swizzle");
    }

    if (!write_image(gs, image.data() + 16u * 16u, 17u) ||
        gs.state().m_transfer_pixels_written != k_pixels ||
        gs.state().m_image_qwc != 33u ||
        read32(gs, k_base + 4u) != 0xa0000002u ||
        read32(gs, k_base + 8u) != 0xa0000042u ||
        read32(gs, k_base + 8192u) != 0xa0000041u ||
        read32(gs, k_base + 8192u + 8u) != 0xa0000082u ||
        read32(gs, k_base + 64u) != 0u)
    {
        return fail("continued DMA, row order, pages or image padding");
    }

    // A rejected batch must roll back its earlier IMAGE, including VRAM
    // and the transfer position, so a subsequent retry can succeed.
    // Use a fresh physical GS page, not the first upload's x=64 page.
    constexpr uint32_t k_second_base = 0x100u * 256u;
    if (!write_register(gs, 0x50u, bitblt(0x100u, 1u, 0u)) ||
        !write_register(gs, 0x52u, trxreg(1u, 1u)) ||
        !write_register(gs, 0x53u, 0u))
    {
        return fail("unable to begin second PSMCT32 transfer");
    }
    std::array<uint8_t, 32> packet{};
    packet[0] = 0x04u;
    packet[1] = 0x03u;
    packet[2] = 0x02u;
    packet[3] = 0x01u;
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x60u; // GS SIGNAL: not implemented
    if (gs.consume(events, 2u, packet.data(), 2u) ||
        read32(gs, k_second_base) != 0u ||
        gs.state().m_transfer_pixels_written != 0u ||
        !write_image(gs, packet.data(), 1u) ||
        read32(gs, k_second_base) != 0x01020304u ||
        gs.state().m_transfer_pixels_written != 1u ||
        read32(gs, k_second_base + 4u) != 0u)
    {
        return fail("failed batch committed VRAM or corrupted retry cursor");
    }

    // VRAM is 4 MiB; the reference uses address wrapping, not a linear
    // allocation beyond the final GS page.
    gs.reset();
    std::array<uint8_t, 48> wrap_data{};
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        const uint32_t word = 0xb0000000u | i;
        wrap_data[i * 4u + 0u] = static_cast<uint8_t>(word);
        wrap_data[i * 4u + 1u] = static_cast<uint8_t>(word >> 8u);
        wrap_data[i * 4u + 2u] = static_cast<uint8_t>(word >> 16u);
        wrap_data[i * 4u + 3u] = static_cast<uint8_t>(word >> 24u);
    }
    if (read32(gs, k_base) != 0u ||
        gs.state().m_transfer_started ||
        !write_register(gs, 0x50u, bitblt(0x3fffu, 1u, 0u)) ||
        !write_register(gs, 0x51u, 0u) ||
        !write_register(gs, 0x52u, trxreg(9u, 1u)) ||
        !write_register(gs, 0x53u, 0u) ||
        !write_image(gs, wrap_data.data(), 3u) ||
        read32(gs, 0x3fffu * 256u) != 0xb0000000u ||
        read32(gs, 0u) != 0xb0000008u)
    {
        return fail("GS reset or 4 MiB VRAM address wrap");
    }

    // An unsupported transfer direction does not accept IMAGE events.
    if (!write_register(gs, 0x53u, 1u) ||
        gs.state().m_transfer_started ||
        write_image(gs, wrap_data.data(), 1u))
    {
        return fail("local-to-host IMAGE was accepted");
    }

    uint32_t unused = 0u;
    if (gs.read_vram32(
            static_cast<uint32_t>(ee::c_ee_gs_state::k_vram_bytes - 3u),
            unused))
    {
        return fail("out-of-range VRAM read");
    }

    std::puts("GS PSMCT32 swizzled VRAM, DMA split and rollback: PASS");
    return 0;
}
