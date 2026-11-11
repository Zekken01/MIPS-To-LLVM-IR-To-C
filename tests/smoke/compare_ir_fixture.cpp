#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace
{
uint32_t make_i(uint8_t opcode, uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t make_r(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) |
           funct;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ee_compare_ir_fixture output.ll\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00120000u;
    const std::vector<uint32_t> words =
    {
        make_i(0x0a, 4, 2, -2),            // slti  $v0,$a0,-2
        make_i(0x0b, 4, 3, -1),            // sltiu $v1,$a0,-1 (sign-extended)
        make_i(0x0a, 6, 6, 1),             // slti  $a2,$a2,1 (rs=rt)
        make_i(0x0b, 7, 7, -1),            // sltiu $a3,$a3,-1 (rs=rt)
        make_r(4, 5, 8, 0, 0x2a),           // slt  $t0,$a0,$a1
        make_r(4, 5, 9, 0, 0x2b),           // sltu $t1,$a0,$a1
        make_r(10, 11, 10, 0, 0x2a),        // slt  $t2,$t2,$t3 (rd=rs)
        make_r(13, 12, 12, 0, 0x2b),        // sltu $t4,$t5,$t4 (rd=rt)
        make_r(4, 5, 0, 0, 0x2a)            // slt  $zero,$a0,$a1
    };

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4u);
        const ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "compare fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.size() != 1 ||
        functions[0].blocks[0].insn_indices.size() != words.size())
    {
        std::fprintf(stderr, "compare fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_scalar_compare_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, "guest_scalar_compare_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
