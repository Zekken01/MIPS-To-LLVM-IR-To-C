#pragma once
#include "ee_opcode.h"
#include <cstdint>
#include <cstddef>

namespace ee {

enum class e_ee_op_type : uint8_t
{
    OP_TYPE_INVALID,
    OP_TYPE_RTYPE,
    OP_TYPE_ITYPE,
    OP_TYPE_JTYPE
};

enum class e_ee_flow : uint8_t
{
    FLOW_NONE,          // sequential fall-through
    FLOW_BRANCH,        // conditional branch
    FLOW_BRANCH_LIKELY,
    FLOW_JUMP,          // unconditional jump (j)
    FLOW_CALL,          // jal / jalr
    FLOW_RETURN,        // jr $ra / ERET (function or exception return)
    FLOW_EXCEPTION,     // SYSCALL / BREAK: exception exit, no fall-through
};

enum class e_ee_valid : uint8_t
{
    VALID,
    UNSUPPORTED,    // recognised encoding but not implemented by the lifter
    INVALID         // malformed / reserved encoding
};

struct s_ee_instruction
{
    uint32_t pc = 0;                // guest pc of this instruction
    uint32_t opcode_word = 0;       // raw 32-bit encoding

    e_ee_opcode opcode = e_ee_opcode::OP_INVALID;
    e_ee_op_type type = e_ee_op_type::OP_TYPE_INVALID;
    e_ee_flow flow = e_ee_flow::FLOW_NONE;
    e_ee_valid valid = e_ee_valid::INVALID;

    bool has_delay_slot = false;
    bool branch_likely = false;
    bool annulled = false;          // branch-likely not-taken annuls the delay slot

    // Register operands
    uint8_t rs = 0;
    uint8_t rt = 0;
    uint8_t rd = 0;
    uint8_t funct = 0;

    uint16_t sa = 0;

    // Immediate / target fields
    int16_t immediate = 0;          // sign-extended 16-bit immediate (I-type)
    uint16_t uimmediate = 0;        // zero-extended 16-bit immediate
    uint32_t jump_target = 0;       // J/JAL absolute target
    int32_t branch_target = 0;      // resolved branch target

    char mnemonic[16] = {};
    char disasm[96] = {};
};

// Build a human-readable disassembly line for a decoded instruction (debug /
// coverage output; does not affect code generation).
void disassemble(s_ee_instruction& insn);

} // namespace ee
