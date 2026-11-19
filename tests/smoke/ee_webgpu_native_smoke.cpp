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

    // Exercise the actual raster compute pipeline, not merely the
    // host-to-local upload/GS display compute pipelines. The 1x1
    // point primitive is written at (2,2), away from the texture upload.
    state.prim = 0u; // point
    state.prmodecont = 1u;
    state.ctx[0].frame = uint64_t{1} << 16u; // FBW=1, PSMCT32
    state.ctx[0].scissor =
        (uint64_t{63} << 16u) | (uint64_t{63} << 48u);
    ps2::gs::Vertex vertex{};
    vertex.x = 32u;
    vertex.y = 32u;
    vertex.z = 0u;
    vertex.r = 0x11u;
    vertex.g = 0x55u;
    vertex.b = 0xaau;
    vertex.a = 0x80u;
    vertex.q = 1.0f;
    ps2::gs::gpu::draw(state, &vertex, 1);
    ps2::gs::gpu::flush();

    const DisplaySource raster_source{
        0u, 1u, ps2::gs::PSMCT32, 0u, 0u, 4u, 4u, false
    };
    rgba.clear();
    width = height = 0u;
    if (!ps2::gs::gpu::display(
            raster_source, rgba, width, height) ||
        width != 4u || height != 4u || rgba.size() != 16u ||
        (rgba[2u + 2u * 4u] & 0x00ffffffu) == 0u)
    {
        std::fprintf(stderr,
            "native GS WebGPU point raster failed: %ux%u pixels=%zu "
            "at(2,2)=%08x\n",
            width, height, rgba.size(),
            rgba.size() >= 11u ? rgba[10u] : 0u);
        return 1;
    }

    std::printf("native GS WebGPU compute: PASS "
                "(upload=%08x point=%08x)\n", rgba[0], rgba[10u]);
    return 0;
}
