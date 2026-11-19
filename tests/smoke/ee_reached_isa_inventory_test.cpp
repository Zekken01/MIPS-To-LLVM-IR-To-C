#include "ee_reached_isa_inventory.h"
#include <cassert>
#include <sstream>
#include <string>

int main()
{
    ee::c_ee_reached_isa_inventory inventory;
    ee::s_ee_instruction_trace_entry entry{};
    entry.m_attempt = 1u;
    entry.m_pc = 0x1000u;
    entry.m_opcode_word = 0x24020001u;  // ADDIU (valid and lowered).
    entry.m_kind = 0u;
    inventory.record(entry);
    inventory.record(entry);  // Same reached PC/word: one unique classification.

    entry.m_attempt = 3u;
    entry.m_pc = 0x1004u;
    entry.m_opcode_word = 0x00003028u;  // MFSA, unsupported.
    inventory.record(entry);
    entry.m_kind = 1u;  // Annulled delay slot is not executed.
    inventory.record(entry);
    entry.m_kind = 3u;  // HLE boundary is not a guest opcode.
    inventory.record(entry);
    entry.m_kind = 2u;  // RAM interpreter committed: include.
    entry.m_pc = 0x1008u;
    entry.m_opcode_word = 0xe0850000u;  // Reserved SC.
    inventory.record(entry);

    assert(inventory.events() == 6u);
    assert(inventory.native_reached() == 3u);
    assert(inventory.totals().m_total == 3u);
    assert(inventory.totals().m_decoder_valid == 1u);
    assert(inventory.totals().m_decoder_unsupported == 1u);
    assert(inventory.totals().m_decoder_invalid == 1u);
    assert(inventory.totals().m_lifter_accepts == 1u);

    std::ostringstream out;
    inventory.write_json(out, 0x1000u, 0x100cu, true);
    const std::string json = out.str();
    assert(json.find("\"truncated\": true") != std::string::npos);
    assert(json.find("\"native_reached_events\": 3") != std::string::npos);
    assert(json.find("\"ram_interpreter_committed_events\": 1") != std::string::npos);
    assert(json.find("\"annulled_branch_slots_excluded\": 1") != std::string::npos);
    assert(json.find("\"hle_boundaries_excluded\": 1") != std::string::npos);
    assert(json.find("\"unique_pc_opcode_pairs\": 3") != std::string::npos);
    assert(json.find("\"reached_instruction_trace_sample\"") != std::string::npos);
    assert(json.find("\"decoder_invalid\": 1") != std::string::npos);
    return 0;
}
