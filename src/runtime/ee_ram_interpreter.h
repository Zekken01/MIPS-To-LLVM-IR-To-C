#pragma once

#include "ee_state.h"
#include <cstdint>

namespace ee
{
class ee_memory;

// Conservatively execute code installed into EE RAM by the game. This is
// a bounded fallback for the static runner, not a full R5900 interpreter.
// Unsupported instructions and unbacked accesses stop with an error.
enum class e_ee_ram_result : uint32_t
{
    RETURNED,
    UNMAPPED,
    UNSUPPORTED,
    BRANCH_IN_DELAY_SLOT,
    INSTRUCTION_BUDGET
};

struct s_ee_ram_execution
{
    e_ee_ram_result m_result = e_ee_ram_result::UNSUPPORTED;
    uint32_t m_pc = 0;
    uint32_t m_opcode = 0;
    uint32_t m_instructions = 0;
};

s_ee_ram_execution ee_execute_ram_callback(
    s_ee_cpu_state* cpu,
    ee_memory* memory,
    uint32_t entry_pc,
    uint32_t return_pc,
    uint32_t max_instructions
);

const char* ee_ram_result_name(e_ee_ram_result result);

} // namespace ee
