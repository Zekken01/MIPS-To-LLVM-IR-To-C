#include "ee_sdbz_pad_return_probe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

int main()
{
    ee::ee_memory memory(8u * 1024u * 1024u);
    ee::c_ee_sdbz_pad_return_probe probe{};
    constexpr uint32_t stack = 0x00020000u;
    constexpr uint32_t result = stack + 0x40u;
    if (probe.observe(0x00109b98u, 1u, stack, 4u, &memory))
    {
        return 1;
    }

    // The original caller combines +2 as the high byte and +3 as
    // the low byte, then complements to recover pressed buttons.
    memory.write8(result + 0u, 0u);
    memory.write8(result + 1u, 0x41u);
    memory.write8(result + 2u, 0xffu);
    memory.write8(result + 3u, 0xffu);
    if (!probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            2u, stack, 4u, &memory))
    {
        return 1;
    }

    // A genuine Cross candidate: raw active-low 0xbfff,
    // inverted to PS2 bit 14 (0x4000).
    memory.write8(result + 2u, 0xbfu);
    if (!probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            3u, stack, 4u, &memory))
    {
        return 1;
    }

    // A failed guest helper status cannot be claimed as an input.
    memory.write8(result + 0u, 1u);
    if (!probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            4u, stack, 4u, &memory) ||
        probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            5u, 0x02000000u, 4u, &memory) ||
        memory.read8(result + 0u) != 1u ||
        memory.read8(result + 1u) != 0x41u ||
        memory.read8(result + 2u) != 0xbfu ||
        memory.read8(result + 3u) != 0xffu)
    {
        return 1;
    }

    FILE* output = std::tmpfile();
    if (output == nullptr)
    {
        return 1;
    }
    probe.write_json(output);
    std::fflush(output);
    std::rewind(output);
    char data[4096]{};
    const size_t count = std::fread(
        data, 1u, sizeof(data) - 1u, output);
    const bool closed = std::fclose(output) == 0;
    if (count == 0u || !closed ||
        std::strstr(data, "\"return_entries\":4") == nullptr ||
        std::strstr(data, "\"ram_samples\":3") == nullptr ||
        std::strstr(data, "\"caller_candidate_samples\":2") == nullptr ||
        std::strstr(data, "\"cross_bit_low_candidates\":1") == nullptr ||
        std::strstr(data, "\"first_cross_candidate\":{") == nullptr ||
        std::strstr(data, "\"raw_buttons\":49151") == nullptr ||
        std::strstr(data, "\"pressed_mask\":16384") == nullptr)
    {
        std::fputs("SDBZ original EE pad return probe regression\n", stderr);
        return 1;
    }

    std::puts("SDBZ original EE pad return read-only probe: PASS");
    return 0;
}
