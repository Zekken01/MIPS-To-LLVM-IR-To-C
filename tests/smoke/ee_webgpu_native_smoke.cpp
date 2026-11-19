// Native GPU execution test; no browser, SDL, display server, or game ELF.
// Uses the original wgpu-native GS compute transfer/display passes.
#include "ps2_webgpu/gs/gpu/gpu.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

int main()
{
    using ps2::gs::gpu::DisplaySource;
    using ps2::gs::gpu::Mode;
    if (!ps2::gs::gpu::init(Mode::primary))
    {
        // No WebGPU adapter available on this runner.
        // CTest may report this as skipped; strict CI can require one.
        std::fprintf(stderr, "native WebGPU initialization failed\n");
        return std::getenv("EE_WEBGPU_REQUIRE_ADAPTER") ? 1 : 77;
    }

    ps2::gs::State state{};
    state.bitbltbuf = uint64_t{1} << 48u; // DBP=0, DBW=1, PSMCT32
    state.trxpos = 0u;
    state.trxreg = 1u | (uint64_t{1} << 32u); // one pixel
    state.trxdir = 0u;
    const uint8_t pixel[16] = {
        0x33u, 0x66u, 0x99u, 0xffu, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    ps2::gs::gpu::image_begin(state);
    ps2::gs::gpu::image_data(pixel, sizeof(pixel));
    ps2::gs::gpu::flush();

    const DisplaySource source{
        0u, 1u, ps2::gs::PSMCT32, 0u, 0u, 1u, 1u, false
    };
    std::vector<uint32_t> rgba;
    uint32_t width = 0u, height = 0u;
    if (!ps2::gs::gpu::display(source, rgba, width, height) ||
        width != 1u || height != 1u || rgba.size() != 1u ||
        (rgba[0] & 0x00ffffffu) == 0u)
    {
        std::fprintf(stderr,
            "native GS WebGPU upload/display failed: %ux%u pixels=%zu"
            " first=%08x\n",
            width, height, rgba.size(), rgba.empty() ? 0u : rgba.front());
        return 1;
    }

    std::printf("native GS WebGPU compute: PASS (pixel=%08x)\n", rgba[0]);
    return 0;
}
