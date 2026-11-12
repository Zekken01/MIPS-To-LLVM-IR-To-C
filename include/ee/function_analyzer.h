#pragma once
#include "ee_instruction.h"
#include "ee_cfg.h"
#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <string>

namespace ee {

// Return statically discoverable instruction PCs after one decoded instruction.
std::set<uint32_t> collect_successors(const s_ee_instruction& ins);

// Build the decoded image's shared CFG leader index once and reuse it for
// multiple per-function analyses over the same instruction map.
std::set<uint32_t> collect_function_block_starts(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& all_entries = {});

// Recover JALR targets whose source register is provably constant on every
// path through the caller CFG. Values that disagree at a merge are discarded.
std::set<uint32_t> collect_static_jalr_targets(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& entry_points,
    const std::vector<uint32_t>& all_entries = {},
    const std::set<uint32_t>* block_starts = nullptr
);

class function_analyzer
{
public:
    std::vector<s_ee_function> analyze(
        const std::map<uint32_t, s_ee_instruction>& insns,
        const std::vector<uint32_t>& entry_points,
        const std::vector<uint32_t>& all_entries = {},
        const std::set<uint32_t>* block_starts = nullptr);

private:
    void split_into_blocks(
        const std::map<uint32_t, s_ee_instruction>& insns,
        uint32_t entry_pc,
        s_ee_function& fn,
        const std::set<uint32_t>& stop_entries,
        const std::set<uint32_t>& block_starts);
    void link_cfg(s_ee_function& fn);
};

} // namespace ee
