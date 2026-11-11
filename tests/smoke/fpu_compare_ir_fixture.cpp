#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

namespace
{
uint32_t make_cop1_s(uint8_t ft, uint8_t fs, uint8_t cc, uint8_t funct)
{
    return (0x11u << 26) |
           (0x10u << 21) |
           (static_cast<uint32_t>(ft) << 16) |
           (static_cast<uint32_t>(fs) << 11) |
           (static_cast<uint32_t>(cc) << 6) |
           funct;
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ee_fpu_compare_ir_fixture output.ll mode\n");
        return 2;
    }

    const char* mode = argv[2];
    uint8_t funct = 0;
    if (std::strcmp(mode, "c_f") == 0) funct = 0x30;
    else if (std::strcmp(mode, "c_eq") == 0) funct = 0x32;
    else if (std::strcmp(mode, "c_lt") == 0) funct = 0x34;
    else if (std::strcmp(mode, "c_le") == 0) funct = 0x36;
    else if (std::strcmp(mode, "cvt_w") == 0) funct = 0x24;
    else if (std::strcmp(mode, "add_s") == 0) funct = 0x00;
    else if (std::strcmp(mode, "sub_s") == 0) funct = 0x01;
    else if (std::strcmp(mode, "mul_s") == 0) funct = 0x02;
    else if (std::strcmp(mode, "div_s") == 0) funct = 0x03;
    else if (std::strcmp(mode, "max_s") == 0) funct = 0x28;
    else if (std::strcmp(mode, "min_s") == 0) funct = 0x29;
    else if (std::strcmp(mode, "abs_s") == 0) funct = 0x05;
    else if (std::strcmp(mode, "neg_s") == 0) funct = 0x07;
    else
    {
        std::fprintf(stderr, "unknown COP1 comparison mode: %s\n", mode);
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00130000u;
    // Comparisons use CC0. Conversion and arithmetic forms use FD.
    const bool writes_fpr6 =
        std::strcmp(mode, "cvt_w") == 0 ||
        std::strcmp(mode, "add_s") == 0 ||
        std::strcmp(mode, "sub_s") == 0 ||
        std::strcmp(mode, "mul_s") == 0 ||
        std::strcmp(mode, "div_s") == 0 ||
        std::strcmp(mode, "max_s") == 0 ||
        std::strcmp(mode, "min_s") == 0 ||
        std::strcmp(mode, "abs_s") == 0 ||
        std::strcmp(mode, "neg_s") == 0;
    const uint8_t destination_or_cc = writes_fpr6 ? 6 : 0;
    const uint32_t word = make_cop1_s(2, 1, destination_or_cc, funct);
    const ee::s_ee_instruction ins = ee::decode(entry_pc, word);
    if (ins.valid != ee::e_ee_valid::VALID ||
        ins.opcode != ee::e_ee_opcode::OP_COP1)
    {
        std::fprintf(stderr, "COP1 comparison was not decoded as VALID: %s\n",
                     ins.mnemonic);
        return 3;
    }

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    insn_map.emplace(entry_pc, ins);

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.size() != 1 ||
        functions[0].blocks[0].insn_indices.size() != 1)
    {
        std::fprintf(stderr, "COP1 comparison fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_fpu_compare_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, "guest_fpu_compare_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
