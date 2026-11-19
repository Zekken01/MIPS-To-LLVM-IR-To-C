#pragma once

#include "ee_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ee
{
// USA SLUS_214.42: 0x102560 polls guest command-state slots. The
// 0x102670 red-sprite loop is entered only after the timeout/recovery path.
// This is passive observation; it does not service the queue or read MMIO.
class c_ee_sdbz_dma_wait_probe
{
public:
    static constexpr uint32_t k_wait_pc = 0x00102560u;
    static constexpr uint32_t k_red_loop_pc = 0x00102670u;
    static constexpr uint32_t k_slot_base = 0x00502350u;
    static constexpr size_t k_slot_words = 32u;
    static constexpr std::array<uint32_t, 7u> k_extra_addresses =
    {
        0x00502630u, 0x00502634u, 0x0050263cu, 0x00502640u,
        0x00503084u, 0x005030acu, 0x005023f0u
    };

    struct s_snapshot
    {
        bool m_observed = false;
        uint32_t m_pc = 0u;
        uint64_t m_dispatch = 0u;
        uint32_t m_thread = 0u;
        std::array<uint32_t, k_slot_words> m_slot_words{};
        std::array<uint32_t, k_extra_addresses.size()> m_extra_words{};

        uint32_t selected_slot() const
        {
            return m_extra_words[5];
        }

        uint32_t polled_slot() const
        {
            // In the ELF, 0x00102578 XORs the active slot with one.
            return selected_slot() ^ 1u;
        }

        uint32_t polled_flag() const
        {
            return polled_slot() == 0u ? flag0() :
                polled_slot() == 1u ? flag1() : 0u;
        }

        uint32_t flag0() const
        {
            // 0x00502368 = base+0x18
            return m_slot_words[6];
        }

        uint32_t flag1() const
        {
            // 0x0050236c = base+0x1c
            return m_slot_words[7];
        }
    };

    // Return true only for the first reached, backed snapshot at each
    // identified PC. For other games / unbacked RAM no sample is invented.
    bool observe(uint32_t pc, uint64_t dispatch, uint32_t thread,
                 ee_memory* memory)
    {
        s_snapshot* target = nullptr;
        if (pc == k_wait_pc && !m_wait.m_observed)
        {
            target = &m_wait;
        }
        else if (pc == k_red_loop_pc && !m_red.m_observed)
        {
            target = &m_red;
        }

        if (target == nullptr || memory == nullptr ||
            !memory->contains(k_slot_base, k_slot_words * sizeof(uint32_t)))
        {
            return false;
        }
        for (uint32_t address : k_extra_addresses)
        {
            if (!memory->contains(address, sizeof(uint32_t)))
            {
                return false;
            }
        }

        s_snapshot sample{};
        sample.m_pc = pc;
        sample.m_dispatch = dispatch;
        sample.m_thread = thread;
        for (size_t i = 0u; i < sample.m_slot_words.size(); ++i)
        {
            sample.m_slot_words[i] =
                memory->read32(k_slot_base + static_cast<uint32_t>(4u * i));
        }
        for (size_t i = 0u; i < sample.m_extra_words.size(); ++i)
        {
            sample.m_extra_words[i] = memory->read32(k_extra_addresses[i]);
        }
        sample.m_observed = true;
        *target = sample;
        return true;
    }

    const s_snapshot& wait() const
    {
        return m_wait;
    }

    const s_snapshot& red_loop() const
    {
        return m_red;
    }

    void write_json(FILE* output) const
    {
        if (output == nullptr)
        {
            return;
        }
        std::fputs("{\"schema\":\"ee-sdbz-dma-wait-v1\","
            "\"observation_unit\":\"native_dispatch_entry\","
            "\"known_elf\":\"SLUS_214.42\","
            "\"limitation\":\"read_only_guest_RAM_not_MMIO_or_causality\","
            "\"wait\":", output);
        write_snapshot_json(output, m_wait);
        std::fputs(",\"red_loop\":", output);
        write_snapshot_json(output, m_red);
        std::fputs("}", output);
    }

private:
    static void write_snapshot_json(FILE* output, const s_snapshot& s)
    {
        if (!s.m_observed)
        {
            std::fputs("null", output);
            return;
        }
        std::fprintf(output,
            "{\"pc\":%u,\"dispatch\":%llu,\"thread\":%u,"
            "\"selected_slot\":%u,\"polled_slot\":%u,"
            "\"polled_flag_valid\":%s,\"polled_flag\":%u,"
            "\"flag0\":%u,\"flag1\":%u,"
            "\"slot_base\":%u,\"slot_words\":[",
            s.m_pc, static_cast<unsigned long long>(s.m_dispatch),
            s.m_thread, s.selected_slot(), s.polled_slot(),
            s.polled_slot() < 2u ? "true" : "false", s.polled_flag(),
            s.flag0(), s.flag1(),
            k_slot_base);
        for (size_t i = 0u; i < s.m_slot_words.size(); ++i)
        {
            std::fprintf(output, "%s%u", i ? "," : "", s.m_slot_words[i]);
        }
        std::fputs("],\"extra_addresses\":[", output);
        for (size_t i = 0u; i < k_extra_addresses.size(); ++i)
        {
            std::fprintf(output, "%s%u", i ? "," : "", k_extra_addresses[i]);
        }
        std::fputs("],\"extra_words\":[", output);
        for (size_t i = 0u; i < s.m_extra_words.size(); ++i)
        {
            std::fprintf(output, "%s%u", i ? "," : "", s.m_extra_words[i]);
        }
        std::fputs("]}", output);
    }

    s_snapshot m_wait{};
    s_snapshot m_red{};
};

} // namespace ee
