#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
constexpr uint32_t k_vif0 = ee::c_ee_dmac::k_vif0_base;
constexpr uint32_t k_vif1 = ee::c_ee_dmac::k_vif1_base;
constexpr uint32_t k_gif = ee::c_ee_dmac::k_gif_base;
constexpr uint32_t k_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);

struct s_capture
{
    bool m_accept = true;
    uint32_t m_calls = 0u;
    uint32_t m_channel = ~uint32_t{0};
    uint32_t m_qwc = 0u;
    std::vector<uint8_t> m_bytes;
};

bool receive(void* user, uint32_t channel, const uint8_t* bytes, uint32_t qwc)
{
    s_capture* sink = static_cast<s_capture*>(user);
    if (sink == nullptr)
    {
        return false;
    }

    ++sink->m_calls;
    sink->m_channel = channel;
    sink->m_qwc = qwc;
    if (!sink->m_accept)
    {
        return false;
    }

    sink->m_bytes.assign(bytes, bytes + static_cast<size_t>(qwc) * 16u);
    return true;
}

int fail(const char* what)
{
    std::fprintf(stderr, "EE device-facing DMA: %s\n", what);
    return 1;
}

uint32_t reg(ee::s_ee_runtime* runtime, uint32_t address)
{
    return ee::ee_mem_read32(runtime, address);
}

void program(ee::s_ee_runtime* runtime, uint32_t base,
             uint32_t madr, uint32_t qwc)
{
    ee::ee_mem_write32(runtime, base + 0x10u, madr);
    ee::ee_mem_write32(runtime, base + 0x20u, qwc);
}
} // namespace

int main()
{
    ee::ee_memory memory(0x2000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);
    s_capture sink{};
    hardware.bind_device_transfer(&sink, receive);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;
    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;

    const ee::s_ee_u128 first{0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull};
    const ee::s_ee_u128 second{0x1716151413121110ull, 0x1f1e1d1c1b1a1918ull};
    ee::ee_mem_write128(&runtime, 0x100u, &first);
    ee::ee_mem_write128(&runtime, 0x110u, &second);

    // GIF channel 2 invokes one callback with the entire QWC stream,
    // in little-endian guest byte order, and latches D_STAT channel 2.
    hardware.reset();
    ee::ee_mem_write32(&runtime, k_stat, 1u << (2u + 16u));
    program(&runtime, k_gif, 0x100u, 2u);
    ee::ee_mem_write32(&runtime, k_gif, k_str);
    if (sink.m_calls != 1u || sink.m_channel != 2u ||
        sink.m_qwc != 2u || sink.m_bytes.size() != 32u ||
        reg(&runtime, k_gif + 0x10u) != 0x120u ||
        reg(&runtime, k_gif + 0x20u) != 0u ||
        (reg(&runtime, k_gif) & k_str) != 0u ||
        (hardware.dmac().status() & (1u << 2u)) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("GIF normal transfer did not reach PATH3 sink or complete");
    }
    for (size_t i = 0; i < sink.m_bytes.size(); ++i)
    {
        if (sink.m_bytes[i] != static_cast<uint8_t>(i))
        {
            return fail("GIF QWC byte order, size or transfer ordering");
        }
    }
    ee::ee_mem_write32(&runtime, k_stat, 1u << 2u);
    if ((cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("GIF completion ACK did not lower IP3");
    }

    // VIF0 is channel 0. VIF1 upload requires CHCR.DIR=1.
    hardware.reset();
    sink.m_calls = 0u;
    program(&runtime, k_vif0, 0x110u, 1u);
    ee::ee_mem_write32(&runtime, k_vif0, k_str);
    if (sink.m_calls != 1u || sink.m_channel != 0u ||
        sink.m_qwc != 1u || sink.m_bytes.size() != 16u ||
        sink.m_bytes[0] != 0x10u || sink.m_bytes[15] != 0x1fu ||
        reg(&runtime, k_vif0 + 0x10u) != 0x120u ||
        (hardware.dmac().status() & 1u) == 0u)
    {
        return fail("VIF0 normal-mode upload");
    }

    hardware.reset();
    sink.m_calls = 0u;
    program(&runtime, k_vif1, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str | 1u);
    if (sink.m_calls != 1u || sink.m_channel != 1u ||
        sink.m_qwc != 1u || reg(&runtime, k_vif1 + 0x10u) != 0x110u ||
        (hardware.dmac().status() & (1u << 1u)) == 0u)
    {
        return fail("VIF1 normal upload with DIR=1");
    }

    // VIF1's opposite direction downloads from the GIF/GS and is NOT
    // equivalent to feeding a VIF command stream into the upload sink.
    hardware.reset();
    sink.m_calls = 0u;
    program(&runtime, k_vif1, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_vif1, k_str);
    if (sink.m_calls != 0u ||
        reg(&runtime, k_vif1 + 0x10u) != 0x100u ||
        reg(&runtime, k_vif1 + 0x20u) != 1u ||
        (reg(&runtime, k_vif1) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("VIF1 reverse/download mode invented a device transfer");
    }

    // With no consumer, a nonempty transfer must stay outstanding,
    // preserving QWC and MADR; it must not fake GS/VIF side effects.
    hardware.reset();
    hardware.bind_device_transfer(nullptr, nullptr);
    program(&runtime, k_gif, 0x100u, 2u);
    ee::ee_mem_write32(&runtime, k_gif, k_str);
    if (reg(&runtime, k_gif + 0x10u) != 0x100u ||
        reg(&runtime, k_gif + 0x20u) != 2u ||
        (reg(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("missing GIF consumer falsely completed DMA");
    }

    // Consumer refusal is another noncompletion: no partial progress.
    hardware.reset();
    sink.m_accept = false;
    sink.m_calls = 0u;
    hardware.bind_device_transfer(&sink, receive);
    program(&runtime, k_gif, 0x100u, 2u);
    ee::ee_mem_write32(&runtime, k_gif, k_str);
    if (sink.m_calls != 1u || reg(&runtime, k_gif + 0x10u) != 0x100u ||
        reg(&runtime, k_gif + 0x20u) != 2u ||
        (reg(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("refusing sink reported successful DMA completion");
    }
    sink.m_accept = true;

    // Missing physical RAM in the SECOND QWC must be detected before
    // the first quad is delivered. Nothing must reach the device sink.
    hardware.reset();
    sink.m_calls = 0u;
    program(&runtime, k_gif, 0x1ff0u, 2u);
    ee::ee_mem_write32(&runtime, k_gif, k_str);
    if (sink.m_calls != 0u ||
        reg(&runtime, k_gif + 0x10u) != 0x1ff0u ||
        reg(&runtime, k_gif + 0x20u) != 2u ||
        (reg(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("invalid source span delivered partial GIF payload");
    }

    // Chain DMA for VIF/GIF is not yet supported, even with a sink
    // installed. Neither source data nor a D_STAT completion is sent.
    hardware.reset();
    sink.m_calls = 0u;
    program(&runtime, k_gif, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, k_gif, k_str | (1u << 2u));
    if (sink.m_calls != 0u || reg(&runtime, k_gif + 0x20u) != 1u ||
        (reg(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("unsupported GIF chain mode fabricated completion");
    }

    // QWC=0 can complete without touching a device. This mirrors the
    // reference's zero-byte deliver() no-op rather than stalling forever.
    hardware.reset();
    hardware.bind_device_transfer(nullptr, nullptr);
    program(&runtime, k_gif, 0x100u, 0u);
    ee::ee_mem_write32(&runtime, k_gif, k_str);
    if ((reg(&runtime, k_gif) & k_str) != 0u ||
        (hardware.dmac().status() & (1u << 2u)) == 0u ||
        reg(&runtime, k_gif + 0x10u) != 0x100u)
    {
        return fail("zero-QWC no-op DMA did not complete");
    }

    // MMIO register width and KSEG1 translated aliases.
    hardware.reset();
    memory.set_kseg_ram_aliases(true);
    program(&runtime, k_gif, 0x100u, 1u);
    ee::ee_mem_write32(&runtime, 0xb000a000u, k_str);
    if (reg(&runtime, 0xb000a000u) != k_str ||
        reg(&runtime, k_gif + 0x20u) != 1u ||
        reg(&runtime, k_vif1 + 0x80u) != 0u)
    {
        return fail("device MMIO KSEG1 aliases or unsupported SADR");
    }
    ee::ee_mem_write8(&runtime, k_gif + 0x20u, 0xffu);
    ee::ee_mem_write16(&runtime, k_gif + 0x20u, 0xffffu);
    ee::ee_mem_write64(&runtime, k_gif + 0x20u, ~uint64_t{0});
    if (reg(&runtime, k_gif + 0x20u) != 1u ||
        ee::ee_mem_read64(&runtime, k_gif + 0x20u) != 0u)
    {
        return fail("invalid GIF register width mutated QWC");
    }

    hardware.reset();
    if (reg(&runtime, k_vif0) != 0u ||
        reg(&runtime, k_vif1) != 0u ||
        reg(&runtime, k_gif) != 0u ||
        hardware.dmac().status() != 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("device channel reset left active registers or interrupts");
    }

    std::puts("EE device normal DMA VIF0/VIF1/GIF host callbacks: PASS");
    return 0;
}
