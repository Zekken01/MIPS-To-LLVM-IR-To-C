#pragma once

// M2.0: classify unique CFG-discovered instruction PCs with the real decoder
// and lifter. Static presence is not evidence of a reached or tested opcode.
#include "ee_instruction.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

namespace ee
{

struct s_ee_isa_bucket
{
    uint64_t m_total = 0u;
    uint64_t m_decoder_valid = 0u;
    uint64_t m_decoder_unsupported = 0u;
    uint64_t m_decoder_invalid = 0u;
    uint64_t m_lifter_accepts = 0u;
    uint64_t m_valid_lift_gap = 0u;
};

class c_ee_isa_inventory
{
public:
    static constexpr size_t k_gap_limit = 32u;

    // Exactly one call per distinct CFG-reachable PC. 'lowered' must be
    // derived from the existing opcode_supported() check (VALID and lift()
    // does not return LOP_UNSUPPORTED); never silently promote a decode.
    void record(const s_ee_instruction& insn, bool lowered)
    {
        const uint32_t primary = insn.opcode_word >> 26u;
        s_ee_isa_bucket& bucket = m_by_primary[primary];
        ++bucket.m_total;
        ++m_totals.m_total;
        if (insn.valid == e_ee_valid::VALID)
        {
            ++bucket.m_decoder_valid;
            ++m_totals.m_decoder_valid;
            if (!lowered)
            {
                ++bucket.m_valid_lift_gap;
                ++m_totals.m_valid_lift_gap;
            }
        }
        else if (insn.valid == e_ee_valid::UNSUPPORTED)
        {
            ++bucket.m_decoder_unsupported;
            ++m_totals.m_decoder_unsupported;
        }
        else
        {
            ++bucket.m_decoder_invalid;
            ++m_totals.m_decoder_invalid;
        }

        if (lowered && insn.valid == e_ee_valid::VALID)
        {
            ++bucket.m_lifter_accepts;
            ++m_totals.m_lifter_accepts;
        }
        else if (m_gaps.size() < k_gap_limit)
        {
            m_gaps.push_back({
                insn.pc, insn.opcode_word,
                insn.valid == e_ee_valid::INVALID ? "invalid" :
                insn.valid == e_ee_valid::UNSUPPORTED ? "unsupported" :
                "valid_lift_gap"
            });
        }
    }

    const s_ee_isa_bucket& totals() const
    {
        return m_totals;
    }

    void write_json(std::ostream& out, bool selected_entries_only) const
    {
        out << "{\n"
            << "    \"schema\": \"ee-m2-cfg-isa-inventory-v1\",\n"
            << "    \"scope\": \""
            << (selected_entries_only
                ? "selected_function_cfgs" : "discovered_function_cfgs")
            << "\",\n"
            << "    \"sampling\": \"unique_cfg_instruction_pcs\",\n"
            << "    \"reached\": \"unmeasured_without_guest_trace\",\n"
            << "    \"tested\": \"not_inferred_from_static_inventory\",\n"
            << "    \"decoder_valid\": " << m_totals.m_decoder_valid << ",\n"
            << "    \"decoder_unsupported\": "
            << m_totals.m_decoder_unsupported << ",\n"
            << "    \"decoder_invalid\": " << m_totals.m_decoder_invalid << ",\n"
            << "    \"lifter_accepts\": " << m_totals.m_lifter_accepts << ",\n"
            << "    \"valid_lift_gap\": " << m_totals.m_valid_lift_gap << ",\n"
            << "    \"total\": " << m_totals.m_total << ",\n"
            << "    \"primary_families\": [";
        bool first = true;
        for (uint32_t primary = 0u; primary < m_by_primary.size(); ++primary)
        {
            const s_ee_isa_bucket& b = m_by_primary[primary];
            if (b.m_total == 0u)
            {
                continue;
            }
            out << (first ? "\n" : ",\n")
                << "      {\"primary\": " << primary
                << ", \"total\": " << b.m_total
                << ", \"decoder_valid\": " << b.m_decoder_valid
                << ", \"decoder_unsupported\": " << b.m_decoder_unsupported
                << ", \"decoder_invalid\": " << b.m_decoder_invalid
                << ", \"lifter_accepts\": " << b.m_lifter_accepts
                << ", \"valid_lift_gap\": " << b.m_valid_lift_gap
                << "}";
            first = false;
        }
        out << (first ? "],\n" : "\n    ],\n")
            << "    \"first_unlowered\": [";
        for (size_t i = 0u; i < m_gaps.size(); ++i)
        {
            const s_gap& gap = m_gaps[i];
            out << (i ? ",\n" : "\n")
                << "      {\"pc\": " << gap.m_pc
                << ", \"word\": " << gap.m_word
                << ", \"primary\": " << (gap.m_word >> 26u)
                << ", \"classification\": \"" << gap.m_classification
                << "\"}";
        }
        out << (m_gaps.empty() ? "]\n" : "\n    ]\n") << "  }";
    }

private:
    struct s_gap
    {
        uint32_t m_pc;
        uint32_t m_word;
        const char* m_classification;
    };

    s_ee_isa_bucket m_totals{};
    std::array<s_ee_isa_bucket, 64u> m_by_primary{};
    std::vector<s_gap> m_gaps;
};

} // namespace ee
