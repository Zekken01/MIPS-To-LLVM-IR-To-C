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

class function_analyzer
{
public:
    std::vector<s_ee_function> analyze(
        const std::map<uint32_t, s_ee_instruction>& insns,
        const std::vector<uint32_t>& entry_points,
        const std::vector<uint32_t>& all_entries = {});

private:
    void split_into_blocks(
        const std::map<uint32_t, s_ee_instruction>& insns,
        uint32_t entry_pc,
        s_ee_function& fn,
        const std::set<uint32_t>& stop_entries);
    void link_cfg(s_ee_function& fn);
};

} // namespace ee
