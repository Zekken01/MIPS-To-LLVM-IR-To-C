#include "ee_instruction.h"
#include "ee_opcode.h"
#include <cstdio>

namespace ee {

// Build a human-readable disassembly line for a decoded instruction. Used for
// debug output and opcode-coverage reports; it does not affect code generation.
void disassemble(s_ee_instruction& insn)
{
    char buf[96] = {};
    switch (insn.opcode)
    {
    case e_ee_opcode::OP_ADDIU:
        std::snprintf(buf, sizeof(buf), "%s $%d, $%d, %d",
            insn.mnemonic, insn.rt, insn.rs, insn.immediate);
        break;
    case e_ee_opcode::OP_DADDIU:
        std::snprintf(buf, sizeof(buf), "%s $%d, $%d, %d",
            insn.mnemonic, insn.rt, insn.rs, insn.immediate);
        break;
    case e_ee_opcode::OP_ANDI:
        std::snprintf(buf, sizeof(buf), "%s $%d, $%d, 0x%x",
            insn.mnemonic, insn.rt, insn.rs, insn.uimmediate);
        break;
    case e_ee_opcode::OP_ORI:
        std::snprintf(buf, sizeof(buf), "%s $%d, $%d, 0x%x",
            insn.mnemonic, insn.rt, insn.rs, insn.uimmediate);
        break;
    case e_ee_opcode::OP_LW:
        std::snprintf(buf, sizeof(buf), "%s $%d, %d($%d)",
            insn.mnemonic, insn.rt, insn.immediate, insn.rs);
        break;
    case e_ee_opcode::OP_SW:
        std::snprintf(buf, sizeof(buf), "%s $%d, %d($%d)",
            insn.mnemonic, insn.rt, insn.immediate, insn.rs);
        break;
    case e_ee_opcode::OP_BEQ:
        std::snprintf(buf, sizeof(buf), "%s $%d, $%d, 0x%x",
            insn.mnemonic, insn.rs, insn.rt, insn.branch_target);
        break;
    case e_ee_opcode::OP_J:
    case e_ee_opcode::OP_JAL:
        std::snprintf(buf, sizeof(buf), "%s 0x%x",
            insn.mnemonic, insn.jump_target);
        break;
    case e_ee_opcode::OP_SPECIAL:
        if (static_cast<e_ee_opcode>(insn.funct) == e_ee_opcode::FUNCT_JR ||
            static_cast<e_ee_opcode>(insn.funct) == e_ee_opcode::FUNCT_JALR)
            std::snprintf(buf, sizeof(buf), "%s $%d", insn.mnemonic, insn.rs);
        else
            std::snprintf(buf, sizeof(buf), "%s", insn.mnemonic);
        break;
    case e_ee_opcode::OP_MMI:
        std::snprintf(buf, sizeof(buf), "%s $%d, $%d, $%d (funct=0x%x)",
            insn.mnemonic, insn.rd, insn.rs, insn.rt, insn.funct);
        break;
    default:
        std::snprintf(buf, sizeof(buf), "%s  // pc=0x%x word=0x%x",
            insn.mnemonic, insn.pc, insn.opcode_word);
        break;
    }
    std::snprintf(insn.disasm, sizeof(insn.disasm), "%s", buf);
}

} // namespace ee

