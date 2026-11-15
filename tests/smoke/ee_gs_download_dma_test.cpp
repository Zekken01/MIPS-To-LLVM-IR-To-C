#include "ee_gs.h"
#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_vif1 = ee::c_ee_dmac::k_vif1_base;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;

int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS download DMA: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t bw,
                uint32_t psm, bool source)
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

bool gs_reg(ee::c_ee_gs_state& gs,
            uint32_t index, uint64_t value)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::packed;
    event.m_register = 0xeu;
    event.m_word1 = index;
    event.m_word0 = value;
    const std::array<uint8_t, 16> dummy{};
    return gs.consume(&event, 1u, dummy.data(), 1u);
}

bool gs_image(ee::c_ee_gs_state& gs,
              const uint8_t* bytes, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, bytes, qwc);
}

bool gs_upload(ee::c_ee_gs_state& gs, uint32_t bp,
               uint32_t psm, uint32_t width, uint32_t height,
               const uint8_t* data, uint32_t qwc)
{
    return gs_reg(gs, 0x50u, bitblt(bp, 2u, psm, false)) &&
        gs_reg(gs, 0x51u, trxpos(0u, 31u, false)) &&
        gs_reg(gs, 0x52u, trxreg(width, height)) &&
        gs_reg(gs, 0x53u, 0u) &&
        gs_image(gs, data, qwc);
}

bool gs_download_setup(ee::c_ee_gs_state& gs,
                       uint32_t bp, uint32_t psm,
                       uint32_t width, uint32_t height)
{
    return gs_reg(gs, 0x50u, bitblt(bp, 2u, psm, true)) &&
        gs_reg(gs, 0x51u, trxpos(0u, 31u, true)) &&
        gs_reg(gs, 0x52u, trxreg(width, height)) &&
        gs_reg(gs, 0x53u, 1u);
}

void dma_program(ee::s_ee_runtime* runtime,
                 uint32_t madr, uint32_t qwc)
{
    ee::ee_mem_write32(runtime, k_vif1 + 0x10u, madr);
    ee::ee_mem_write32(runtime, k_vif1 + 0x20u, qwc);
}

uint32_t dma_reg(ee::s_ee_runtime* runtime, uint32_t offset)
{
    return ee::ee_mem_read32(runtime, k_vif1 + offset);
}

bool reject(void*, uint32_t, uint8_t*, uint32_t)
{
    return false;
}
} // namespace

int main()
{
    ee::ee_memory memory(0x2000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x180u;
    std::array<uint8_t, 32> input{};
    for (uint32_t i = 0u; i < 21u; ++i)
    {
        input[i] = static_cast<uint8_t>(0x40u + i);
    }
    if (!gs_upload(gs, k_bp, 1u, 7u, 1u,
                   input.data(), 2u) ||
        !gs_download_setup(gs, k_bp, 1u, 7u, 1u))
    {
        return fail("PSMCT24 source upload or readback setup");
    }

    // Without a download producer, VIF1 reverse DMA must not read
    // RAM as if it were a forward VIF transfer, or fake completion.
    for (uint32_t i = 0u; i < 48u; ++i)
    {
        memory.write8(0x100u + i, 0xaau);
    }
    hardware.reset();
    dma_program(&runtime, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x100u ||
        dma_reg(&runtime, 0x20u) != 1u ||
        (dma_reg(&runtime, 0u) & k_str) == 0u ||
        hardware.dmac().status() != 0u ||
        gs.state().m_download_bytes_read != 0u ||
        memory.read8(0x100u) != 0xaau)
    {
        return fail("missing reverse-DMA callback committed EE RAM");
    }

    // A refusing producer must also preserve EE RAM and DMA
    // registers; GS readback cursor cannot advance on rejection.
    hardware.reset();
    hardware.bind_device_download(nullptr, reject);
    dma_program(&runtime, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x100u ||
        dma_reg(&runtime, 0x20u) != 1u ||
        (dma_reg(&runtime, 0u) & k_str) == 0u ||
        hardware.dmac().status() != 0u ||
        memory.read8(0x100u) != 0xaau ||
        gs.state().m_download_bytes_read != 0u)
    {
        return fail("refusing download callback committed partial DMA");
    }

    // A malformed EE destination range is rejected BEFORE invoking
    // the GS producer, so neither guest memory nor GS state changes.
    hardware.reset();
    hardware.bind_device_download(
        &gs, &ee::c_ee_gs_state::dma_download
    );
    dma_program(&runtime, 0x1ff0u, 2u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x1ff0u ||
        dma_reg(&runtime, 0x20u) != 2u ||
        (dma_reg(&runtime, 0u) & k_str) == 0u ||
        gs.state().m_download_bytes_read != 0u ||
        hardware.dmac().status() != 0u ||
        memory.read8(0x1ff0u) != 0u)
    {
        return fail("invalid DMA destination advanced GS readback");
    }

    // Two 1-QWC reverse VIF1 DMAs retrieve a 7-pixel, 24-bit
    // rectangle. Byte 16 belongs to pixel 5's second byte; the
    // 11 padding bytes at the end of QWC 2 must be zero.
    hardware.reset();
    dma_program(&runtime, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x110u ||
        dma_reg(&runtime, 0x20u) != 0u ||
        (dma_reg(&runtime, 0u) & k_str) != 0u ||
        hardware.dmac().status() != (1u << 1u) ||
        gs.state().m_download_bytes_read != 16u ||
        gs.state().m_download_qwc != 1u ||
        !gs.state().m_download_active)
    {
        return fail("first reverse VIF1 DMA did not complete correctly");
    }
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        if (memory.read8(0x100u + i) != input[i])
        {
            return fail("first VIF1 DMA byte ordering differs from GS");
        }
    }

    ee::ee_mem_write32(&runtime, k_vif1 + 0x20u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x120u ||
        dma_reg(&runtime, 0x20u) != 0u ||
        gs.state().m_download_bytes_read != 32u ||
        gs.state().m_download_qwc != 2u ||
        gs.state().m_download_active ||
        (dma_reg(&runtime, 0u) & k_str) != 0u)
    {
        return fail("second reverse VIF1 DMA did not finish rectangle");
    }
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        const uint8_t expected = i < 5u ? input[16u + i] : 0u;
        if (memory.read8(0x110u + i) != expected)
        {
            return fail("24-bit carry or padded second DMA incorrect");
        }
    }
    if (memory.read8(0x120u) != 0xaau ||
        memory.read8(0x12fu) != 0xaau)
    {
        return fail("readback DMA wrote beyond destination QWC range");
    }

    // Forward VIF1 (DIR=1) must still require the separate existing
    // RAM-to-device sink. A bound GS download source is not a
    // substitute for VIF command execution.
    hardware.reset();
    dma_program(&runtime, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str | 1u);
    if (gs.state().m_download_bytes_read != 32u ||
        dma_reg(&runtime, 0x10u) != 0x100u ||
        dma_reg(&runtime, 0x20u) != 1u ||
        (dma_reg(&runtime, 0u) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("VIF1 forward transfer bypassed missing VIF consumer");
    }

    // An invalid GS transfer also rejects reverse DMA atomically.
    hardware.reset();
    gs.reset();
    dma_program(&runtime, 0x140u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x140u ||
        dma_reg(&runtime, 0x20u) != 1u ||
        (dma_reg(&runtime, 0u) & k_str) == 0u ||
        hardware.dmac().status() != 0u ||
        memory.read8(0x140u) != 0u)
    {
        return fail("missing TRXDIR=1 source falsely completed DMA");
    }

    // A zero-QWC transfer is a valid no-op even without a producer.
    hardware.reset();
    hardware.bind_device_download(nullptr, nullptr);
    dma_program(&runtime, 0x180u, 0u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (dma_reg(&runtime, 0x10u) != 0x180u ||
        dma_reg(&runtime, 0x20u) != 0u ||
        (dma_reg(&runtime, 0u) & k_str) != 0u ||
        (hardware.dmac().status() & (1u << 1u)) == 0u)
    {
        return fail("zero-QWC VIF1 reverse DMA did not complete as no-op");
    }

    std::puts("GS reverse VIF1 DMA to EE RAM, carry, rollback: PASS");
    return 0;
}
