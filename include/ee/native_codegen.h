#pragma once
#include "ee_instruction.h"
#include "instruction_lifter.h"
#include <string>
#include <vector>
#include <map>

namespace ee {

// Emits a self-contained C++ translation unit that implements one lifted guest
// function using the EE runtime ABI and memory helpers. Compiling this TU with
// a host C++ compiler and linking against the EE runtime produces a native
// object/executable that can be run and differentially compared against the
// reference interpreter (PLAN.md section 6).
//
// The emitter targets straight-line (single basic block) functions:
// control-transfer instructions end the emitted region and are reported as
// unsupported by this stage (they are already covered by the in-process
// differential tests).
class native_codegen
{
public:
    native_codegen() = default;

    // Emit a complete C++ program implementing `ops` as function `func_name`
    // with entry guest pc `entry_pc`. The program sets up initial CPU state
    // from `init_gpr` (reg index -> value) and memory from `init_mem`
    // (address -> 32-bit value), runs the function, and prints the final
    // architectural state to stdout as hex lines.
    std::string emit_program(
        const std::vector<s_lifted_op>& ops,
        uint32_t entry_pc,
        const char* func_name,
        const std::map<uint8_t, uint64_t>& init_gpr,
        const std::map<uint32_t, uint32_t>& init_mem);

    // Emit a callable C++ function implementing `ops` as `func_name`
    // with entry guest pc `entry_pc`. The function has signature:
    //   uint32_t func_name(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime* rt);
    // It executes the lifted ops using `cpu` and `rt` pointers (no globals),
    // and returns the exit reason. Suitable for compiling to object files
    // and linking multiple functions into a single test executable.
    std::string emit_function_body(
        const std::vector<s_lifted_op>& ops,
        uint32_t entry_pc,
        const char* func_name);

private:
    std::string out_;
    int temp_ = 0;

    std::string fresh();
    void touched_regs(const std::vector<s_lifted_op>& ops,
                      std::vector<uint8_t>& out) const;
    void emit_op_body(const s_lifted_op& op);
};

} // namespace ee
