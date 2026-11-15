#pragma once

#include "ee_gif.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// GS register-state and PSMCT32/PSMCT24 host-to-local transfer consumer.
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
    // Returns false for out-of-range raw VRAM accesses.
    bool read_vram32(uint32_t offset, uint32_t& value) const;

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
        uint8_t m_bytes = 4u; // PSMCT24 preserves VRAM high byte
    };

    // GS PSMCT32/24: 64x32 pages, 8 KiB per page, 256-byte blocks.
    // Addresses wrap at 4 MiB, matching ps2-web-recomp GS memory.
    static uint32_t psmct32_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

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
