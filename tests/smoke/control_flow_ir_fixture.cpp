#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

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
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ee_control_flow_ir_fixture case output.ll\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00100000u;
    const std::string mode = argv[1];
    uint32_t control_word = 0;
    uint32_t delay_word = 0;
    const char* function_name = nullptr;

    if (mode == "jr")
    {
        // JR $v0; the delay slot overwrites $v0. The dispatcher must receive
        // the target captured before the delay slot executes.
        control_word = make_r(2, 0, 0, 0, 0x08);
        delay_word = make_i(0x09, 0, 2, 0x1234);
        function_name = "guest_jr_delay_test";
    }
    else if (mode == "jrra")
    {
        // JR $ra returns to the captured target even though its delay slot
        // overwrites the architectural link register.
        control_word = make_r(31, 0, 0, 0, 0x08);
        delay_word = make_i(0x09, 0, 31, 0x1234);
        function_name = "guest_jr_return_test";
    }
    else if (mode == "jal")
    {
        // JAL writes $ra before its delay slot; the slot reads that link value.
        constexpr uint32_t target = entry_pc + 0x2000u;
        control_word = (0x03u << 26) | ((target >> 2) & 0x03ffffffu);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_jal_delay_test";
    }
    else if (mode == "jalr")
    {
        // JALR $t0,($t0): capture the target before rd aliases rs and receives
        // PC+8. The delay slot then reads the newly written link in $t0.
        control_word = make_r(8, 0, 8, 0, 0x09);
        delay_word = make_i(0x09, 8, 9, 1);
        function_name = "guest_jalr_delay_test";
    }
    else if (mode == "beq_taken")
    {
        // BEQ must sample $v0 before its delay slot increments it.
        control_word = make_i(0x04, 2, 0, 4);
        delay_word = make_i(0x09, 2, 2, 1);
        function_name = "guest_beq_taken_delay_test";
    }
    else if (mode == "beq_not_taken")
    {
        // BEQ must remain not-taken even though the delay slot makes $v0 zero.
        control_word = make_i(0x04, 2, 0, 4);
        delay_word = make_i(0x09, 0, 2, 0);
        function_name = "guest_beq_not_taken_delay_test";
    }
    else if (mode == "bltzal")
    {
        // BLTZAL $v0, +4; its delay slot executes on both branch outcomes and
        // observes the unconditional PC+8 link value.
        control_word = make_i(0x01, 2, 0x10, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bltzal_delay_test";
    }
    else if (mode == "bltzall")
    {
        // BLTZALL has an annulled delay slot on the not-taken path. The link
        // is still written unconditionally before the branch outcome.
        control_word = make_i(0x01, 2, 0x12, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bltzall_delay_test";
    }
    else if (mode == "bgezal")
    {
        control_word = make_i(0x01, 2, 0x11, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bgezal_delay_test";
    }
    else if (mode == "bgezall")
    {
        control_word = make_i(0x01, 2, 0x13, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bgezall_delay_test";
    }
    else
    {
        std::fprintf(stderr, "unknown control-flow fixture case: %s\n", mode.c_str());
        return 2;
    }

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    const ee::s_ee_instruction control = ee::decode(entry_pc, control_word);
    const ee::s_ee_instruction delay = ee::decode(entry_pc + 4u, delay_word);

    if (control.valid != ee::e_ee_valid::VALID || !control.has_delay_slot)
    {
        std::fprintf(stderr, "%s control transfer failed to decode: %s (valid=%u)\n",
                     mode.c_str(), control.mnemonic, static_cast<unsigned>(control.valid));
        return 3;
    }
    if (delay.valid != ee::e_ee_valid::VALID)
    {
        std::fprintf(stderr, "%s delay-slot instruction failed to decode: %s\n",
                     mode.c_str(), delay.mnemonic);
        return 4;
    }

    insn_map.emplace(control.pc, control);
    insn_map.emplace(delay.pc, delay);

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.size() != 1 ||
        functions[0].blocks[0].insn_indices.size() != 2)
    {
        std::fprintf(stderr, "%s fixture CFG did not retain its delay slot\n", mode.c_str());
        return 5;
    }

    llvm::llvm_context context(mode + "_delay_slot_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, function_name);

    if (!context.write_ir_to_file(argv[2]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[2]);
        return 6;
    }

    return 0;
}
