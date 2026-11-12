#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t ENTRY_PC = 0x00130000u;

uint32_t make_cop2(uint8_t rs, uint8_t rt, uint8_t rd)
{
    return (0x12u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11);
}

uint32_t make_addiu(uint8_t rs, uint8_t rt, uint16_t immediate)
{
    return (0x09u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           immediate;
}

uint32_t make_beq(uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (0x04u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t make_jr(uint8_t rs)
{
    return (static_cast<uint32_t>(rs) << 21) | 0x08u;
}

bool build_words(const std::string& mode, std::vector<uint32_t>& words)
{
    uint8_t cop2_rs = 0;
    if (mode.rfind("qmfc2", 0) == 0)
        cop2_rs = 0x01;
    else if (mode.rfind("cfc2", 0) == 0)
        cop2_rs = 0x02;
    else if (mode.rfind("qmtc2", 0) == 0)
        cop2_rs = 0x05;
    else if (mode.rfind("ctc2", 0) == 0)
        cop2_rs = 0x06;
    else
        return false;

    const uint32_t transfer = make_cop2(cop2_rs, 8, 6);
    if (mode == "qmfc2" || mode == "qmtc2" ||
        mode == "cfc2" || mode == "ctc2")
    {
        words = {transfer};
        return true;
    }

    if (mode == "qmfc2_delay" || mode == "qmtc2_delay" ||
        mode == "cfc2_delay" || mode == "ctc2_delay")
    {
        words = {
            make_beq(0, 0, 1), // target is the instruction after the delay slot
            transfer,
            make_jr(31),
            0u
        };
        return true;
    }

    if (mode == "qmtc2_after_addiu" || mode == "ctc2_after_addiu")
    {
        words = {
            make_addiu(0, 8, 0x1234),
            transfer
        };
        return true;
    }

    if (mode == "cfc2_then_addiu")
    {
        words = {
            make_cop2(0x02, 9, 6), // CFC2 r9, VI6
            make_addiu(9, 10, 1)  // consumes the value written by the helper
        };
        return true;
    }

    return false;
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr,
                     "usage: ee_vu0_transfer_ir_fixture output.ll mode\n");
        return 2;
    }

    const std::string mode = argv[2];
    std::vector<uint32_t> words;
    if (!build_words(mode, words))
    {
        std::fprintf(stderr, "unknown VU0 transfer mode: %s\n", argv[2]);
        return 2;
    }

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = ENTRY_PC + static_cast<uint32_t>(i * 4);
        const ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr,
                         "VU0 fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {ENTRY_PC});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "VU0 transfer fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_vu0_transfer_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map,
                              "guest_vu0_transfer_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
