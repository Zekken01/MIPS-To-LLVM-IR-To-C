#include "ee_webgpu_bridge.h"
#include "ps2_webgpu/gs/gpu/gpu.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int s_draws = 0;
int s_images = 0;
int s_upload_bytes = 0;
int s_local_copies = 0;
int s_clut_loads = 0;
int s_texflushes = 0;
ps2::gs::State s_last_state{};
ps2::gs::Vertex s_last_vertex{};
ps2::gs::gpu::DisplaySource s_last_source{};

void fail(const char* message)
{
    std::fprintf(stderr, "native WebGPU event adapter: %s\n", message);
}
} // namespace

namespace ps2::gs
{
uint32_t g_setup_generation = 0u;
}

namespace ps2::gs::gpu
{
bool init(Mode mode)
{
    return mode == Mode::primary;
}

void draw(const State& state, const Vertex* vertices, int count)
{
    if (vertices != nullptr && count > 0)
    {
        ++s_draws;
        s_last_state = state;
        s_last_vertex = vertices[0];
    }
}

void image_begin(const State&)
{
    ++s_images;
}

void image_data(const uint8_t*, size_t bytes)
{
    s_upload_bytes += static_cast<int>(bytes);
}

void local_copy(const State&)
{
    ++s_local_copies;
}

void load_clut(uint64_t, uint64_t)
{
    ++s_clut_loads;
}

void texflush()
{
    ++s_texflushes;
}

bool display(
    const DisplaySource& src, std::vector<uint32_t>& rgba,
    uint32_t& width, uint32_t& height)
{
    s_last_source = src;
    width = src.width;
    height = src.height;
    rgba.assign(static_cast<size_t>(width) * height, 0xff336699u);
    return true;
}
}

int main()
{
    ee::c_ee_webgpu_bridge bridge{};
    if (!bridge.init() || !bridge.active())
    {
        fail("cannot initialize native-only mode");
        return 1;
    }
    ee::s_ee_gif_event events[5]{};
    events[0].m_type = ee::e_ee_gif_event_type::tag;
    events[1].m_type = ee::e_ee_gif_event_type::primitive;
    events[1].m_word0 = 0u; // point primitive
    events[2].m_type = ee::e_ee_gif_event_type::reglist;
    events[2].m_register = 0x01u; // RGBAQ, 1.0 Q
    events[2].m_word0 = 0x3f80000044332211ull;
    events[3].m_type = ee::e_ee_gif_event_type::reglist;
    events[3].m_register = 0x4cu; // FRAME_1
    events[3].m_word0 = (uint64_t{1} << 16u);
    events[4].m_type = ee::e_ee_gif_event_type::reglist;
    events[4].m_register = 0x05u; // XYZ2 32-bit Z
    events[4].m_word0 = 0x0000001100300020ull;
    uint8_t bytes[16]{};
    bridge.consume(events, 5u, bytes, 1u);
    if (s_draws != 1 || bridge.rendered_primitives() != 1u ||
        s_last_vertex.x != 0x20u || s_last_vertex.y != 0x30u ||
        s_last_vertex.z != 0x11u ||
        s_last_vertex.r != 0x11u || s_last_vertex.g != 0x22u ||
        s_last_vertex.b != 0x33u || s_last_vertex.a != 0x44u)
    {
        fail("point vertex or RGBAQ state was not forwarded");
        return 1;
    }
    bridge.consume(nullptr, 1u, bytes, 1u);
    if (s_draws != 1)
    {
        fail("invalid mirror input was not discarded");
        return 1;
    }

    ee::s_ee_gif_event transfers[5]{};
    for (auto& e : transfers)
    {
        e.m_type = ee::e_ee_gif_event_type::reglist;
    }
    transfers[0].m_register = 0x50u;
    transfers[0].m_word0 = 1ull << 48u;
    transfers[1].m_register = 0x52u;
    transfers[1].m_word0 = 4u | (uint64_t{2} << 32u);
    transfers[2].m_register = 0x53u;
    transfers[2].m_word0 = 0u; // host-to-local
    transfers[3].m_type = ee::e_ee_gif_event_type::image;
    transfers[3].m_byte_offset = 0u;
    transfers[3].m_qwc = 1u;
    transfers[4].m_register = 0x3fu; // TEXFLUSH
    bridge.consume(transfers, 5u, bytes, 1u);
    if (s_images != 1 || s_upload_bytes != 16 || s_texflushes != 1)
    {
        fail("native upload/texflush path was not forwarded");
        return 1;
    }
    ee::s_ee_gif_event copy{};
    copy.m_type = ee::e_ee_gif_event_type::reglist;
    copy.m_register = 0x53u;
    copy.m_word0 = 2u;
    ee::c_ee_webgpu_bridge::committed_gif_events(
        &bridge, &copy, 1u, bytes, 1u);
    if (s_local_copies != 1)
    {
        fail("native local VRAM copy path was not forwarded");
        return 1;
    }

    ee::s_ee_gs_display_registers display{};
    display.m_pmode = 1u;
    display.m_dispfb[0] = 1ull << 9u;
    display.m_display[0] = (63ull << 32u) | (31ull << 44u);
    std::vector<uint32_t> output;
    uint32_t width = 0u, height = 0u;
    if (!bridge.readback(display, 2u, output, width, height) ||
        width != 64u || height != 32u || output.size() != 2048u ||
        s_last_source.fbw != 1u ||
        s_last_source.width != width ||
        s_last_source.height != height ||
        output.front() != 0xff336699u)
    {
        fail("native compute-display source was not selected");
        return 1;
    }
    display.m_dispfb[0] |= 0x13ull << 15u;
    if (bridge.readback(display, 2u, output, width, height))
    {
        fail("unsupported indexed display source was accepted");
        return 1;
    }

    std::puts("EE native WebGPU event adapter: PASS");
    return 0;
}
