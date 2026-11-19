#include "ee_sdbz_dma_wait_probe.h"

#include <cstdint>
#include <cstring>
#include <cstdio>

int main()
{
    ee::ee_memory memory(8u * 1024u * 1024u);
    ee::c_ee_sdbz_dma_wait_probe probe{};
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&memory);
    const auto write_vif1 = [&](uint32_t offset, uint32_t value) -> bool
    {
        const ee::s_ee_u128 word{value, 0u};
        return ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_vif1_base + offset, 4u, &word);
    };
    // Unsupported guest VIF1 source-chain starts cannot fabricate
    // completion. The passive probe must preserve STR and D_STAT.
    if (!write_vif1(0x10u, 0x1000u) ||
        !write_vif1(0x20u, 1u) ||
        !write_vif1(0x30u, 0x2000u) ||
        !write_vif1(0x00u, 0x105u) ||
        dmac.vif1_channel_state().m_chcr != 0x105u ||
        dmac.pending() != 0u)
    {
        std::fputs("M2 SDBZ unsupported VIF1 chain was retired\n", stderr);
        return 1;
    }

    // The two alternating guest slots must be sampled without changing
    // the value held in guest RAM or generating a device read.
    memory.write32(0x00502368u, 0u);
    memory.write32(0x0050236cu, 7u);
    memory.write32(0x005030acu, 1u);
    memory.write32(0x00502354u, 1024u);
    if (probe.observe(0x001025a4u, 10u, 1u, &memory, &dmac) ||
        !probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                       11u, 1u, &memory, &dmac) ||
        probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                      12u, 1u, &memory, &dmac))
    {
        std::fputs("M2 SDBZ DMA wait did not capture one-shot entry\n", stderr);
        return 1;
    }
    if (!probe.wait().m_observed ||
        probe.wait().m_dispatch != 11u ||
        probe.wait().flag0() != 0u ||
        probe.wait().flag1() != 7u ||
        probe.wait().selected_slot() != 1u ||
        probe.wait().polled_slot() != 0u ||
        probe.wait().polled_flag() != 0u ||
        probe.pending_wait().m_observed ||
        probe.wait().m_slot_words[1] != 1024u ||
        !probe.wait().m_vif1_observed ||
        probe.wait().m_vif1_chcr != 0x105u ||
        probe.wait().m_vif1_madr != 0x1000u ||
        probe.wait().m_vif1_qwc != 1u ||
        probe.wait().m_vif1_tadr != 0x2000u ||
        probe.wait().m_dmac_status != 0u)
    {
        std::fputs("M2 SDBZ wait snapshot mismatch\n", stderr);
        return 1;
    }

    // The first call was a successful wait. The next enters a pending
    // opposite-slot wait, which is the one we need to diagnose.
    memory.write32(0x00502368u, 1u);
    if (!write_vif1(0x30u, 0x2010u))
    {
        std::fputs("M2 SDBZ VIF1 TADR update refused\n", stderr);
        return 1;
    }
    if (!probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                       13u, 1u, &memory, &dmac) ||
        probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                      14u, 1u, &memory, &dmac) ||
        !probe.pending_wait().m_observed ||
        probe.pending_wait().m_dispatch != 13u ||
        probe.pending_wait().polled_slot() != 0u ||
        probe.pending_wait().polled_flag() != 1u ||
        probe.pending_wait().m_vif1_tadr != 0x2010u ||
        probe.wait().m_vif1_tadr != 0x2000u ||
        probe.wait().flag0() != 0u)
    {
        std::fputs("M2 SDBZ first pending wait was lost\n", stderr);
        return 1;
    }

    memory.write32(0x00502368u, 2u);
    memory.write32(0x0050236cu, 9u);
    memory.write32(0x005030acu, 0u);
    if (!probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_red_loop_pc,
                       1024u, 1u, &memory, &dmac) ||
        probe.red_loop().flag0() != 2u ||
        probe.red_loop().flag1() != 9u ||
        probe.red_loop().selected_slot() != 0u ||
        probe.red_loop().polled_slot() != 1u ||
        probe.red_loop().polled_flag() != 9u ||
        probe.wait().flag0() != 0u ||
        probe.pending_wait().flag0() != 1u ||
        memory.read32(0x00502368u) != 2u ||
        memory.read32(0x0050236cu) != 9u)
    {
        std::fputs("M2 SDBZ red-loop snapshot or RAM was altered\n", stderr);
        return 1;
    }
    FILE* output = std::tmpfile();
    if (output == nullptr)
    {
        std::fputs("could not create temporary probe JSON file\n", stderr);
        return 1;
    }
    probe.write_json(output);
    std::fflush(output);
    std::rewind(output);
    char data[4096]{};
    const size_t count = std::fread(data, 1u, sizeof(data) - 1u, output);
    const bool closed = std::fclose(output) == 0;
    if (count == 0u || !closed ||
        std::strstr(data, "\"selected_slot\":1") == nullptr ||
        std::strstr(data, "\"selected_slot\":0") == nullptr ||
        std::strstr(data, "\"pending_wait\":{") == nullptr ||
        std::strstr(data, "\"vif1\":{\"chcr\":261") == nullptr ||
        std::strstr(data, "\"schema\":\"ee-sdbz-dma-wait-v1\"") == nullptr)
    {
        std::fputs("M2 SDBZ DMA wait JSON missing expected values\n", stderr);
        return 1;
    }
    std::puts("M2 SDBZ DMA wait read-only probe: PASS");
    return 0;
}
