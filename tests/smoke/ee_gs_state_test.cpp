#include "ee_gs.h"
#include "ee_gif.h"
#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_gif = ee::c_ee_dmac::k_gif_base;
constexpr uint32_t k_d_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;
constexpr uint32_t k_gif_bit = 1u << 2u;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);

int fail(const char* what)
{
    std::fprintf(stderr, "EE GS register sink: %s\n", what);
    return 1;
}

uint64_t gif_tag(
    uint32_t loop, uint32_t flg, uint32_t nreg,
    bool pre = false, uint32_t prim = 0u)
{
    return static_cast<uint64_t>(loop & 0x7fffu) |
        (uint64_t{1} << 15u) |
        (static_cast<uint64_t>(pre ? 1u : 0u) << 46u) |
        (static_cast<uint64_t>(prim & 0x7ffu) << 47u) |
        (static_cast<uint64_t>(flg & 3u) << 58u) |
        (static_cast<uint64_t>(nreg & 15u) << 60u);
}

void quad(
    ee::s_ee_runtime* runtime, uint32_t address,
    uint64_t low, uint64_t high)
{
    const ee::s_ee_u128 value{low, high};
    ee::ee_mem_write128(runtime, address, &value);
}

void tag(
    ee::s_ee_runtime* runtime, uint32_t address,
    uint32_t nloop, uint32_t flg, uint32_t nreg,
    uint64_t regs, bool pre = false, uint32_t prim = 0u)
{
    quad(runtime, address, gif_tag(nloop, flg, nreg, pre, prim), regs);
}

void ad(
    ee::s_ee_runtime* runtime, uint32_t address,
    uint32_t reg, uint64_t value)
{
    quad(runtime, address, value, reg);
}

void dma(
    ee::s_ee_runtime* runtime, uint32_t address, uint32_t qwc)
{
    ee::ee_mem_write32(runtime, k_gif + 0x10u, address);
    ee::ee_mem_write32(runtime, k_gif + 0x20u, qwc);
    ee::ee_mem_write32(runtime, k_gif, k_str);
}

uint32_t mmio(ee::s_ee_runtime* runtime, uint32_t address)
{
    return ee::ee_mem_read32(runtime, address);
}

} // namespace

int main()
{
    ee::ee_memory memory(0x8000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware;
    ee::c_ee_gif_path3 gif;
    ee::c_ee_gs_state gs;

    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);
    hardware.bind_device_transfer(&gif, ee::c_ee_gif_path3::dma_transfer);
    gif.bind_event_sink(&gs, ee::c_ee_gs_state::gif_events);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;
    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;
    ee::ee_mem_write32(&runtime, k_d_stat, 1u << 18u);

    // Full path: first GIFtag selects PRE/PRIM, packed RGBAQ uses Q=1;
    // STQ changes Q, and the second RGBAQ uses the new Q. UV masks its
    // two 14-bit coords; packed XYZ2 with ADC=1 goes to GS XYZ3.
    // The PACKED packet spans two complete DMA transfers.
    tag(&runtime, 0x100u, 1u, 0u, 5u, 0x53121ull, true, 0x25u);
    quad(&runtime, 0x110u, (uint64_t{0x22} << 32u) | 0x11u,
         (uint64_t{0x44} << 32u) | 0x33u);
    quad(&runtime, 0x120u, 0x0123456789abcdefull, 0x3f000000u);
    quad(&runtime, 0x130u, (uint64_t{0x66} << 32u) | 0x55u,
         (uint64_t{0x88} << 32u) | 0x77u);
    quad(&runtime, 0x140u, (uint64_t{0x4001} << 32u) | 0x7fffu, 0u);
    quad(&runtime, 0x150u, (uint64_t{0xabcd} << 32u) | 0x1234u,
         (uint64_t{1} << 47u) | 0x12345678u);

    dma(&runtime, 0x100u, 3u);
    if (!gs.was_written(0x00u) ||
        gs.register_value(0x00u) != 0x25u ||
        gs.register_value(0x01u) != 0x3f80000044332211ull ||
        gs.register_value(0x02u) != 0x0123456789abcdefull ||
        gs.state().m_q_bits != 0x3f000000u ||
        gs.state().m_tag_count != 1u ||
        !gif.state().m_active ||
        mmio(&runtime, k_gif + 0x10u) != 0x130u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("PRE/PRIM, PACKED RGBAQ and STQ transfer");
    }
    ee::ee_mem_write32(&runtime, k_d_stat, k_gif_bit);
    dma(&runtime, 0x130u, 3u);
    if (gif.state().m_active ||
        gs.register_value(0x01u) != 0x3f00000088776655ull ||
        gs.register_value(0x03u) != 0x00013fffu ||
        !gs.was_written(0x0du) ||
        gs.register_value(0x0du) != 0x12345678abcd1234ull ||
        gs.was_written(0x05u) ||
        gs.state().m_vertex_count != 1u ||
        gs.state().m_tag_count != 1u ||
        mmio(&runtime, k_gif + 0x20u) != 0u)
    {
        return fail("split PACKED continuation, UV or XYZ3 ADC select");
    }

    // A+D register writes configure a host-to-local transfer. The GS
    // consumer retains IMAGE bytes and commits PSMCT32 swizzled VRAM.
    tag(&runtime, 0x200u, 4u, 0u, 1u, 0xeull);
    const uint64_t transfer_bitblt =
        (uint64_t{0x20} << 32u) | (uint64_t{1} << 48u);
    ad(&runtime, 0x210u, 0x50u, transfer_bitblt);
    ad(&runtime, 0x220u, 0x51u, 0x00000000aabbccdduLL);
    ad(&runtime, 0x230u, 0x52u, uint64_t{4} | (uint64_t{3} << 32u));
    ad(&runtime, 0x240u, 0x53u, 0u);
    tag(&runtime, 0x250u, 2u, 2u, 1u, 0u);
    quad(&runtime, 0x260u, 0x0706050403020100ull,
         0x0f0e0d0c0b0a0908ull);
    quad(&runtime, 0x270u, 0x1716151413121110ull,
         0x1f1e1d1c1b1a1918ull);
    tag(&runtime, 0x280u, 1u, 3u, 1u, 0u);
    quad(&runtime, 0x290u, 0x2726252423222120ull,
         0x2f2e2d2c2b2a2928ull);

    dma(&runtime, 0x200u, 7u);
    if (!gs.was_written(0x50u) ||
        !gs.was_written(0x51u) ||
        !gs.was_written(0x52u) ||
        !gs.was_written(0x53u) ||
        gs.register_value(0x50u) != transfer_bitblt ||
        gs.register_value(0x51u) != 0xaabbccddu ||
        gs.register_value(0x52u) != (uint64_t{4} | (uint64_t{3} << 32u)) ||
        gs.register_value(0x53u) != 0u ||
        gs.state().m_image_qwc != 1u ||
        gs.state().m_image_data.size() != 16u ||
        !gif.state().m_active ||
        gs.state().m_q_bits != 0x3f800000u)
    {
        return fail("A+D transfer setup and first IMAGE payload");
    }
    dma(&runtime, 0x270u, 1u);
    if (gif.state().m_active ||
        gs.state().m_image_qwc != 2u ||
        gs.state().m_image_data.size() != 32u)
    {
        return fail("IMAGE payload continuation across DMA boundary");
    }
    dma(&runtime, 0x280u, 2u);
    if (gif.state().m_active ||
        gs.state().m_image_qwc != 3u ||
        gs.state().m_image_data.size() != 48u)
    {
        return fail("IMAGE2 payload append");
    }
    for (uint32_t i = 0; i < 48u; ++i)
    {
        if (gs.state().m_image_data[i] != static_cast<uint8_t>(i))
        {
            return fail("image data byte order and transfer aggregation");
        }
    }

    // All three IMAGE packets wrote the first 12 pixels, in raster order.
    uint32_t pixel = 0u;
    if (!gs.read_vram32(0x20u * 256u, pixel) ||
        pixel != 0x03020100u ||
        gs.state().m_transfer_pixels_written != 12u)
    {
        return fail("PSMCT32 VRAM upload from continued IMAGE data");
    }

    // TEX2 writes merge selected bitfields into TEX0, preserving its
    // remaining bits; context state and PRMODECONT are retained.
    constexpr uint64_t k_tex2_mask = 0xffffffe003f00000ull;
    const uint64_t tex0 = 0x0123456789abcdefull;
    const uint64_t tex2 = 0xfedcba9876543210ull;
    tag(&runtime, 0x300u, 4u, 0u, 1u, 0xeull);
    ad(&runtime, 0x310u, 0x06u, tex0);
    ad(&runtime, 0x320u, 0x16u, tex2);
    ad(&runtime, 0x330u, 0x1au, 1u);
    ad(&runtime, 0x340u, 0x40u, 0x12345678ull);
    dma(&runtime, 0x300u, 5u);
    if (gs.register_value(0x06u) !=
            ((tex0 & ~k_tex2_mask) | (tex2 & k_tex2_mask)) ||
        gs.register_value(0x16u) != tex2 ||
        gs.register_value(0x1au) != 1u ||
        gs.register_value(0x40u) != 0x12345678ull)
    {
        return fail("TEX2 masked TEX0 merge or GS context register capture");
    }

    // REGLIST sends two 64-bit writes per qword; the odd high half
    // is padding. Its ST register does NOT modify packed STQ's Q state.
    hardware.reset();
    gif.reset();
    gs.reset();
    tag(&runtime, 0x400u, 1u, 1u, 3u, 0x210ull);
    quad(&runtime, 0x410u, 0x33u, 0x1122334455667788ull);
    quad(&runtime, 0x420u, 0xabcd1234u, 0xdeadbeefu);
    dma(&runtime, 0x400u, 3u);
    if (gif.state().m_active ||
        gs.register_value(0x00u) != 0x33u ||
        gs.register_value(0x01u) != 0x1122334455667788ull ||
        gs.register_value(0x02u) != 0xabcd1234u ||
        gs.state().m_q_bits != 0x3f800000u ||
        gs.state().m_tag_count != 1u)
    {
        return fail("REGLIST GS state or odd-halfword padding");
    }

    // Reject SIGNAL with no GS interrupt model. Even the earlier valid
    // FRAME_1 write in the SAME DMA batch must be rolled back.
    hardware.reset();
    gif.reset();
    gs.reset();
    tag(&runtime, 0x500u, 2u, 0u, 1u, 0xeull);
    ad(&runtime, 0x510u, 0x4cu, 0x55aa55aaull);
    ad(&runtime, 0x520u, 0x60u, 0x1234u);
    dma(&runtime, 0x500u, 3u);
    if (gs.was_written(0x4cu) ||
        gs.state().m_tag_count != 0u ||
        gif.state().m_active ||
        mmio(&runtime, k_gif + 0x10u) != 0x500u ||
        mmio(&runtime, k_gif + 0x20u) != 3u ||
        (mmio(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("unsupported SIGNAL committed partial state or DMA");
    }

    // Retry the same packet after changing the unsupported GS write.
    ad(&runtime, 0x520u, 0x4du, 0x11221122ull);
    ee::ee_mem_write32(&runtime, k_gif, 0u);
    dma(&runtime, 0x500u, 3u);
    if (!gs.was_written(0x4cu) ||
        !gs.was_written(0x4du) ||
        gs.register_value(0x4cu) != 0x55aa55aaull ||
        gs.register_value(0x4du) != 0x11221122ull ||
        gs.state().m_tag_count != 1u ||
        (hardware.dmac().status() & k_gif_bit) == 0u)
    {
        return fail("DMA retry after rejecting unsupported GS signal");
    }

    // IMAGE without a configured TRXDIR cannot be treated as uploaded.
    hardware.reset();
    gif.reset();
    gs.reset();
    tag(&runtime, 0x600u, 1u, 2u, 1u, 0u);
    quad(&runtime, 0x610u, 0x1234ull, 0x5678ull);
    dma(&runtime, 0x600u, 2u);
    if (gs.state().m_tag_count != 0u ||
        !gs.state().m_image_data.empty() ||
        gif.state().m_active ||
        mmio(&runtime, k_gif + 0x20u) != 2u ||
        (mmio(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("IMAGE without GS transfer setup was falsely accepted");
    }

    // Direct batch validation rejects out-of-range payload offsets
    // and HWREG without an active host-to-local transfer.
    ee::s_ee_gif_event invalid{};
    invalid.m_type = ee::e_ee_gif_event_type::packed;
    invalid.m_register = 0xeu;
    invalid.m_word0 = 0x12345678ull;
    invalid.m_word1 = 0x54u; // HWREG
    uint8_t qword[16]{};
    if (gs.consume(&invalid, 1u, qword, 1u) ||
        gs.state().m_tag_count != 0u)
    {
        return fail("HWREG without TRXDIR=0 was accepted");
    }
    invalid.m_type = ee::e_ee_gif_event_type::image;
    invalid.m_qwc = 2u;
    if (gs.consume(&invalid, 1u, qword, 1u) ||
        gs.state().m_tag_count != 0u)
    {
        return fail("out-of-range IMAGE data was accepted");
    }

    hardware.reset();
    gif.reset();
    gs.reset();
    if (gs.state().m_q_bits != 0x3f800000u ||
        gs.state().m_tag_count != 0u ||
        gs.was_written(0x50u) ||
        gs.state().m_vertex_count != 0u ||
        !gs.state().m_image_data.empty() ||
        gs.state().m_transfer_started ||
        gs.state().m_transfer_pixels_written != 0u)
    {
        return fail("GS state reset did not release captured registers/data");
    }

    // The retail game's boot path starts normal GIF DMA with CHCR=0x101
    // (DIR|STR), then polls STR at 0x1000A000. The actual GS PATH3
    // sink must consume the packet before the DMAC clears STR or
    // announces a channel-2 completion.
    tag(&runtime, 0x700u, 1u, 0u, 1u, 0xeull);
    ad(&runtime, 0x710u, 0x4cu, 0x11223344ull);
    ee::ee_mem_write32(&runtime, k_gif + 0x10u, 0x700u);
    ee::ee_mem_write32(&runtime, k_gif + 0x20u, 2u);
    ee::ee_mem_write32(&runtime, k_gif, 0x101u);
    if (!gs.was_written(0x4cu) ||
        gs.register_value(0x4cu) != 0x11223344ull ||
        gs.state().m_tag_count != 1u ||
        mmio(&runtime, k_gif + 0x10u) != 0x720u ||
        mmio(&runtime, k_gif + 0x20u) != 0u ||
        (mmio(&runtime, k_gif) & k_str) != 0u ||
        (hardware.dmac().status() & k_gif_bit) == 0u)
    {
        return fail("retail CHCR=0x101 GIF DMA did not reach the GS sink");
    }

    // A rejected SIGNAL packet may not complete the same CHCR mode.
    hardware.reset();
    gif.reset();
    gs.reset();
    tag(&runtime, 0x740u, 1u, 0u, 1u, 0xeull);
    ad(&runtime, 0x750u, 0x60u, 0x1u);
    ee::ee_mem_write32(&runtime, k_gif + 0x10u, 0x740u);
    ee::ee_mem_write32(&runtime, k_gif + 0x20u, 2u);
    ee::ee_mem_write32(&runtime, k_gif, 0x101u);
    if (gs.state().m_tag_count != 0u ||
        mmio(&runtime, k_gif + 0x10u) != 0x740u ||
        mmio(&runtime, k_gif + 0x20u) != 2u ||
        (mmio(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("unsupported CHCR=0x101 GIF packet fabricated DMA completion");
    }

    std::puts("EE GS register capture via DMA/GIF with atomic IMAGE: PASS");
    return 0;
}
