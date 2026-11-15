#pragma once

#include "ee_gif.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// GS register-state and PSMCT32/24/16/16S plus PSMT8/PSMT4 upload consumer.
// It does not rasterize primitives, present frames or implement GS IRQs.
// Based on ps2-web-recomp/runtime/src/gs/gs.cpp::write/write_packed.
struct s_ee_gs_state
{
    std::array<uint64_t, 128> m_registers{};
    std::array<bool, 128> m_written{};
    uint32_t m_q_bits = 0x3f800000u; // GIFtag resets the STQ Q to 1.0
    uint64_t m_tag_count = 0;
    uint64_t m_vertex_count = 0; // diagnostic only; no rasterization
    uint32_t m_image_qwc = 0;
    std::vector<uint8_t> m_image_data; // bounded diagnostic transfer capture
    bool m_transfer_started = false;
    uint32_t m_transfer_pixels_written = 0; // across IMAGE events
    uint8_t m_transfer_pending_size = 0; // PSMCT24 incomplete pixel
    std::array<uint8_t, 3> m_transfer_pending{};

    // Snapshot of the GS internal CLUT, not a view of current VRAM.
    // TEX0/TEX2 CLD controls when entries are reloaded from GS memory.
    std::array<uint32_t, 512> m_clut{};
    std::array<bool, 512> m_clut_valid{};
    uint32_t m_clut_cbp0 = 0xffffffffu;
    uint32_t m_clut_cbp1 = 0xffffffffu;
    uint64_t m_clut_load_count = 0;
};

// Transactional consumer for c_ee_gif_path3::t_event_callback.
// Unsupported stateful GS operations (SIGNAL/FINISH/LABEL, HWREG,
// unknown register addresses) are rejected instead of being executed.
class c_ee_gs_state
{
public:
    static constexpr size_t k_max_image_bytes = 4u * 1024u * 1024u;
    static constexpr size_t k_vram_bytes = 4u * 1024u * 1024u;

    // GS VRAM is a separate 4 MiB allocation, not EE RAM.
    const std::vector<uint8_t>& vram() const;
    bool read_vram8(uint32_t offset, uint8_t& value) const;
    // Returns false for out-of-range raw VRAM accesses.
    bool read_vram16(uint32_t offset, uint16_t& value) const;
    bool read_vram32(uint32_t offset, uint32_t& value) const;

    // Return raw 32-bit or 16-bit CPSM palette words without RGBA
    // expansion, alpha conversion, filtering, or texture rasterization.
    // Only entries loaded by a supported TEX0/TEX2 CLD are readable.
    bool read_clut_entry(uint32_t slot, uint32_t& value) const;
    bool read_indexed_clut_value(
        uint32_t context, uint32_t x, uint32_t y, uint32_t& value
    ) const;

    void reset();
    const s_ee_gs_state& state() const;
    bool was_written(uint32_t reg) const;
    uint64_t register_value(uint32_t reg) const;

    // Accepts or rejects one entire GIF event batch. The source buffer
    // is borrowed for the duration of this call only. On failure, neither
    // the GS state, the preceding GIF parser state, nor the DMA registers
    // are committed. No GPU commands are issued.
    bool consume(
        const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );

    static bool gif_events(
        void* user, const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );

private:
    struct s_vram_write
    {
        uint32_t m_offset = 0;
        uint32_t m_value = 0;
        uint8_t m_bytes = 4u; // 0 = nibble write; otherwise 1..4 bytes
        uint8_t m_shift = 0u; // nibble position: 0 (low) or 4 (high)
    };

    // GS PSMCT32/24: 64x32 pages, 8 KiB per page, 256-byte blocks.
    // Addresses wrap at 4 MiB, matching ps2-web-recomp GS memory.
    static uint32_t psmct32_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // PSMCT16 and PSMCT16S use 64x64 pages with different 16x8
    // block permutations and a shared 16-bit column permutation.
    static uint32_t psmct16_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y,
        uint32_t psm
    );

    // PSMT8: 128x64 pages, 16x16 blocks and byte-lane swizzling.
    // DBW is still measured in units of 64 pixels.
    static uint32_t psmt8_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // PSMT4: 128x128 pages, 32x16 blocks and nibble-lane swizzling.
    // The returned index is in nibbles, wrapped over 4 MiB GS VRAM.
    static uint32_t psmt4_nibble_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // Load a CLUT snapshot on TEX0/TEX2 writes. Staged IMAGE writes
    // are overlaid for same-batch palette uploads before final commit.
    bool load_clut(
        s_ee_gs_state& state, uint64_t tex0,
        const std::vector<s_vram_write>& writes
    ) const;

    static bool write_register(
        s_ee_gs_state& state, uint32_t reg, uint64_t value
    );
    static bool write_packed(
        s_ee_gs_state& state, uint32_t reg, uint64_t low, uint64_t high
    );

    s_ee_gs_state m_state{};
    std::vector<uint8_t> m_vram = std::vector<uint8_t>(k_vram_bytes, 0u);
};
} // namespace ee
