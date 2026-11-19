#include "ee_sdbz_dma_wait_probe.h"

#include <cstdint>
#include <cstring>
#include <cstdio>

int main()
{
    ee::ee_memory memory(8u * 1024u * 1024u);
    ee::c_ee_sdbz_dma_wait_probe probe{};

    // The two alternating guest slots must be sampled without changing
    // the value held in guest RAM or generating a device read.
    memory.write32(0x00502368u, 0u);
    memory.write32(0x0050236cu, 7u);
    memory.write32(0x005030acu, 1u);
    memory.write32(0x00502354u, 1024u);
    if (probe.observe(0x001025a4u, 10u, 1u, &memory) ||
        !probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                       11u, 1u, &memory) ||
        probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                      12u, 1u, &memory))
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
        probe.wait().m_slot_words[1] != 1024u)
    {
        std::fputs("M2 SDBZ wait snapshot mismatch\n", stderr);
        return 1;
    }

    // The first call was a successful wait. The next enters a pending
    // opposite-slot wait, which is the one we need to diagnose.
    memory.write32(0x00502368u, 1u);
    if (!probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                       13u, 1u, &memory) ||
        probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_wait_pc,
                      14u, 1u, &memory) ||
        !probe.pending_wait().m_observed ||
        probe.pending_wait().m_dispatch != 13u ||
        probe.pending_wait().polled_slot() != 0u ||
        probe.pending_wait().polled_flag() != 1u ||
        probe.wait().flag0() != 0u)
    {
        std::fputs("M2 SDBZ first pending wait was lost\n", stderr);
        return 1;
    }

    memory.write32(0x00502368u, 2u);
    memory.write32(0x0050236cu, 9u);
    memory.write32(0x005030acu, 0u);
    if (!probe.observe(ee::c_ee_sdbz_dma_wait_probe::k_red_loop_pc,
                       1024u, 1u, &memory) ||
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
        std::strstr(data, "\"schema\":\"ee-sdbz-dma-wait-v1\"") == nullptr)
    {
        std::fputs("M2 SDBZ DMA wait JSON missing expected values\n", stderr);
        return 1;
    }
    std::puts("M2 SDBZ DMA wait read-only probe: PASS");
    return 0;
}
