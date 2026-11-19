#pragma once

// Classify only instructions actually recorded by a bounded native trace.
// Kind 1 is an annulled branch slot; kind 3 is host HLE, not a guest
// instruction. Do not extrapolate a bounded prefix to an entire boot.
#include "ee_decoder.h"
#include "ee_isa_inventory.h"
#include "ee_runtime.h"
#include "instruction_lifter.h"
#include <cstdint>
#include <ostream>
#include <set>
#include <utility>

namespace ee
{

class c_ee_reached_isa_inventory
{
public:
    void record(const s_ee_instruction_trace_entry& event)
    {
        ++m_events;
        if (event.m_kind == 1u)
        {
            ++m_annulled_slots;
        }
        else if (event.m_kind == 3u)
        {
            ++m_hle_boundaries;
        }
        else if (event.m_kind == 0u || event.m_kind == 2u)
        {
            if (event.m_kind == 0u)
            {
                ++m_native_reached;
            }
            else
            {
                ++m_ram_interpreter_committed;
            }
            if (m_unique_words.insert({
                    event.m_pc, event.m_opcode_word}).second)
            {
                const s_ee_instruction decoded =
                    decode(event.m_pc, event.m_opcode_word);
                const bool accepted =
                    decoded.valid == e_ee_valid::VALID &&
                    lift(decoded).kind != LOP_UNSUPPORTED;
                m_inventory.record(decoded, accepted);
            }
        }
        else
        {
            ++m_unknown_kind;
        }
    }

    void write_json(std::ostream& out, uint32_t first_pc,
                    uint32_t last_pc, bool truncated) const
    {
        out << "{\n"
            << "  \"schema\": \"ee-m2-reached-isa-trace-v1\",\n"
            << "  \"sampling\": \"bounded_append_only_guest_instruction_trace\",\n"
            << "  \"trace_first_pc\": " << first_pc << ",\n"
            << "  \"trace_last_pc\": " << last_pc << ",\n"
            << "  \"truncated\": " << (truncated ? "true" : "false") << ",\n"
            << "  \"recorded_events\": " << m_events << ",\n"
            << "  \"native_reached_events\": " << m_native_reached << ",\n"
            << "  \"ram_interpreter_committed_events\": "
            << m_ram_interpreter_committed << ",\n"
            << "  \"annulled_branch_slots_excluded\": "
            << m_annulled_slots << ",\n"
            << "  \"hle_boundaries_excluded\": " << m_hle_boundaries << ",\n"
            << "  \"unknown_kind_excluded\": " << m_unknown_kind << ",\n"
            << "  \"unique_pc_opcode_pairs\": "
            << m_unique_words.size() << ",\n"
            << "  \"isa_inventory\": ";
        m_inventory.write_json(
            out, "reached_instruction_trace_sample",
            "observed_kind_0_native_or_kind_2_interpreter");
        out << ",\n"
            << "  \"limitations\": ["
            << "\"no_cycle_timestamps\", "
            << "\"sample_may_not_cover_entire_run\", "
            << "\"classification_does_not_prove_architectural_correctness\", "
            << "\"hLE_and_annulled_slots_not_executed_instructions\""
            << "]\n"
            << "}\n";
    }

    uint64_t events() const
    {
        return m_events;
    }

    uint64_t native_reached() const
    {
        return m_native_reached;
    }

    const s_ee_isa_bucket& totals() const
    {
        return m_inventory.totals();
    }

private:
    uint64_t m_events = 0u;
    uint64_t m_native_reached = 0u;
    uint64_t m_ram_interpreter_committed = 0u;
    uint64_t m_annulled_slots = 0u;
    uint64_t m_hle_boundaries = 0u;
    uint64_t m_unknown_kind = 0u;
    std::set<std::pair<uint32_t, uint32_t>> m_unique_words;
    c_ee_isa_inventory m_inventory;
};

} // namespace ee
