#pragma once

#include "ee_gs.h"

#include <cstdint>
#include <vector>

namespace ee
{

// Explicit privileged GS display-register snapshot. Kept separate from
// GIF A+D registers and all uncalibrated scanout/timing infrastructure.
struct s_ee_gs_display_registers
{
    uint64_t m_pmode = 0u;
    uint64_t m_smode2 = 0u;
    uint64_t m_dispfb[2]{};
    uint64_t m_display[2]{};
};

class c_ee_gs_display
{
public:
    // Pure read-only VRAM display extraction, based on the reference
    // ps2-web-recomp display() circuit-selection and magnification rules.
    // The registers MUST come from guest GS writes in production. This
    // method does not synthesize a VBlank, framebuffer or present event.
    // No outputs change when the register state is unsupported.
    static bool readback(
        const c_ee_gs_state& gs,
        const s_ee_gs_display_registers& registers,
        uint32_t crt_mode,
        std::vector<uint32_t>& pixels,
        uint32_t& width,
        uint32_t& height
    );
};
} // namespace ee
