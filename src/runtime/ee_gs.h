#pragma once

#include "ee_gif.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// GS command-state capture, NOT a GPU/GS renderer. It records supported
// register writes and stages host-to-local IMAGE payload bytes without
// performing VRAM writes, primitives, signals or hardware IRQs.
// Based on ps2-web-recomp/runtime/src/gs/gs.cpp::write/write_packed.
struct s_ee_gs_state
{
    std::array<uint64_t, 128> m_registers{};
    std::array<bool, 128> m_written{};
    uint32_t m_q_bits = 0x3f800000u; // GIFtag resets the STQ Q to 1.0
    uint64_t m_tag_count = 0;
    uint64_t m_vertex_count = 0; // diagnostic only; no rasterization
    uint32_t m_image_qwc = 0;
    std::vector<uint8_t> m_image_data; // last host-to-local upload stream
};

// Transactional consumer for c_ee_gif_path3::t_event_callback.
// Unsupported stateful GS operations (SIGNAL/FINISH/LABEL, HWREG,
// unknown register addresses) are rejected instead of being executed.
class c_ee_gs_state
{
public:
    static constexpr size_t k_max_image_bytes = 4u * 1024u * 1024u;

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
    static bool write_register(
        s_ee_gs_state& state, uint32_t reg, uint64_t value
    );
    static bool write_packed(
        s_ee_gs_state& state, uint32_t reg, uint64_t low, uint64_t high
    );

    s_ee_gs_state m_state{};
};
} // namespace ee
