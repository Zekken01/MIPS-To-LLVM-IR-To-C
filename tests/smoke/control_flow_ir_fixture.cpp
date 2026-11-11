#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
uint32_t make_r(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) |
           funct;
}

uint32_t make_i(uint8_t opcode, uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t make_j(uint8_t opcode, uint32_t target)
{
    return (static_cast<uint32_t>(opcode) << 26) |
           ((target >> 2) & 0x03ffffffu);
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ee_control_flow_ir_fixture output.ll mode\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00100000u;
    constexpr uint32_t direct_target = 0x00101000u;
    const std::string mode = argv[2];
    std::vector<uint32_t> words;

    if (mode == "jal")
    {
        words =
        {
            make_j(0x03, direct_target),       // jal direct_target
            make_r(31, 0, 17, 0, 0x21)         // addu $s1,$ra,$zero
        };
    }
    else if (mode == "jalr")
    {
        words =
        {
            make_r(8, 0, 16, 0, 0x09),         // jalr $s0,$t0
            make_r(16, 0, 17, 0, 0x21)         // delay slot reads newly-written $s0
        };
    }
    else if (mode == "jalr_alias")
    {
        words =
        {
            make_r(8, 0, 8, 0, 0x09),          // jalr $t0,$t0 (snapshot old target)
            make_r(8, 0, 17, 0, 0x21)          // delay slot reads link in $t0
        };
    }
    else if (mode == "jr_ra")
    {
        words =
        {
            make_r(31, 0, 0, 0, 0x08),         // jr $ra
            make_i(0x09, 0, 31, 0x1234)        // delay slot overwrites $ra
        };
    }
    else if (mode == "jr_t9")
    {
        words =
        {
            make_r(25, 0, 0, 0, 0x08),         // jr $t9
            make_i(0x09, 0, 25, 0x4321)        // delay slot overwrites $t9
        };
    }
    else
    {
        std::fprintf(stderr, "unknown control-flow mode: %s\n", argv[2]);
        return 2;
    }

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4);
        const ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "control-flow fixture analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_control_flow_runtime_test");
    llvm::module_builder builder(context, 0);
    builder.emit_function_cfg(functions[0], insn_map, "guest_control_flow_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
