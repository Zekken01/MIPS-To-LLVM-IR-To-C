#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <map>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ee_control_flow_ir_fixture output.ll\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00100000u;

    // JR $v0 is deliberately unsupported until dynamic dispatch is implemented.
    // Its delay-slot instruction is nevertheless architecturally mandatory.
    const uint32_t jr_v0 = (2u << 21) | 0x08u;
    const uint32_t delay_addiu_t0 =
        (0x09u << 26) | (8u << 16) | 0x1234u;

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    const ee::s_ee_instruction branch = ee::decode(entry_pc, jr_v0);
    const ee::s_ee_instruction delay = ee::decode(entry_pc + 4u, delay_addiu_t0);

    if (branch.valid != ee::e_ee_valid::UNSUPPORTED ||
        branch.flow != ee::e_ee_flow::FLOW_JUMP ||
        !branch.has_delay_slot)
    {
        std::fprintf(stderr, "JR $v0 did not decode as an unsupported delayed jump\n");
        return 3;
    }

    if (delay.valid != ee::e_ee_valid::VALID)
    {
        std::fprintf(stderr, "delay-slot ADDIU failed to decode\n");
        return 4;
    }

    insn_map.emplace(branch.pc, branch);
    insn_map.emplace(delay.pc, delay);

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.size() != 1 ||
        functions[0].blocks[0].insn_indices.size() != 2)
    {
        std::fprintf(stderr, "dynamic JR fixture CFG did not retain its delay slot\n");
        return 5;
    }

    llvm::llvm_context context("indirect_jr_delay_slot_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, "guest_indirect_jr_delay_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 6;
    }

    return 0;
}
