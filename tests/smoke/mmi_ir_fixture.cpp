#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

uint32_t make_mmi(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (0x1cu << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) |
           funct;
}
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "usage: ee_mmi_ir_fixture output.ll [opt_level]\n");
        return 2;
    }
    const int opt_level = argc == 3 ? std::atoi(argv[2]) : 0;
    if (opt_level < 0 || opt_level > 3)
    {
        std::fprintf(stderr, "opt_level must be between 0 and 3\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00100000u;
    const std::vector<uint32_t> words =
    {
        make_i(0x09, 0, 4, 0x1234),        // addiu $a0,$zero,0x1234
        make_i(0x09, 0, 14, 0x5678),       // addiu $t6,$zero,0x5678
        make_i(0x04, 0, 0, 2),             // beq $zero,$zero, target (split CFG)
        0x00000000u,                       // delay-slot nop
        make_i(0x09, 0, 14, 0x9999),       // not-taken path must not execute
        make_mmi(4, 0, 14, 0, 0x11),       // mthi1 $a0; rd field is unused
        make_mmi(0, 0, 5, 0, 0x10),        // mfhi1 $a1
        make_mmi(5, 0, 0, 0, 0x13),        // mtlo1 $a1 (must see pending MFHI1)
        make_mmi(0, 0, 6, 0, 0x12),        // mflo1 $a2
        make_mmi(8, 9, 7, 0x12, 0x09),     // pand $a3,$t0,$t1
        make_mmi(8, 9, 10, 0x13, 0x09),    // pxor $t2,$t0,$t1
        make_mmi(8, 9, 11, 0x12, 0x29),    // por $t3,$t0,$t1
        make_mmi(8, 9, 12, 0x13, 0x29),    // pnor $t4,$t0,$t1
        make_mmi(8, 9, 8, 0x12, 0x09),     // pand $t0,$t0,$t1 (rd aliases rs)
        make_mmi(8, 9, 13, 0x13, 0x09)     // pxor $t5,$t0,$t1 sees new $t0
    };

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4);
        ee::s_ee_instruction ins = ee::decode(pc, words[i]);
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
        std::fprintf(stderr, "fixture function analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("mmi_transfer_runtime_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], insn_map, "guest_mmi_transfer_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
