#pragma once
#include "ee_instruction.h"
#include <cstdint>
#include <vector>
#include <set>
#include <string>

namespace ee {

// A single basic block: a maximal straight-line sequence of decoded
// instructions ending in a control-transfer instruction (or at the end of the
// function). All instructions execute if the block is entered.
struct s_ee_basic_block
{
    uint32_t start_pc = 0;          // guest pc of first instruction
    uint32_t end_pc = 0;            // guest pc of last instruction (+4)
    uint32_t fallthrough_pc = 0;    // pc of sequential successor (if any)
    uint32_t target_pc = 0;         // branch/jump target (if any)
    bool has_delay_slot = false;    // last instruction has a delay slot
    bool branch_likely = false;     // last instruction is branch-likely
    bool is_terminator = false;     // block ends in return/jump (no fallthrough)

    std::vector<size_t> insn_indices;   // indices into the function's insn list
    std::set<uint32_t> successors;      // pcs of successor blocks
    std::set<uint32_t> predecessors;    // pcs of predecessor blocks
};

// A lifted guest function with its control-flow graph.
struct s_ee_function
{
    uint32_t entry_pc = 0;
    std::string name;
    std::vector<s_ee_basic_block> blocks;
};

} // namespace ee
