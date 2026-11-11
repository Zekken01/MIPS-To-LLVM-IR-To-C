#include "function_analyzer.h"
#include "ee_opcode.h"
#include <algorithm>
#include <set>

namespace ee {

std::set<uint32_t> collect_successors(const s_ee_instruction& ins)
{
    std::set<uint32_t> out;
    if (ins.valid == e_ee_valid::INVALID)
        return out;

    const uint32_t after_delay = ins.pc + (ins.has_delay_slot ? 8u : 4u);
    if (ins.has_delay_slot)
        out.insert(ins.pc + 4u);

    switch (ins.flow)
    {
    case e_ee_flow::FLOW_BRANCH:
    case e_ee_flow::FLOW_BRANCH_LIKELY:
        out.insert(static_cast<uint32_t>(ins.branch_target));
        out.insert(after_delay);
        break;
    case e_ee_flow::FLOW_JUMP:
        // J has an immediate target; JR's register target is dynamic.
        if (ins.opcode == e_ee_opcode::OP_J)
            out.insert(ins.jump_target);
        break;
    case e_ee_flow::FLOW_CALL:
        // JAL has an immediate target; JALR is resolved by the dispatcher.
        if (ins.opcode == e_ee_opcode::OP_JAL)
            out.insert(ins.jump_target);
        out.insert(after_delay);
        break;
    case e_ee_flow::FLOW_RETURN:
        break;
    case e_ee_flow::FLOW_EXCEPTION:
        // SYSCALL/BREAK leave the translated block for the exception handler,
        // but the handler may resume at EPC+4. Discover that continuation so
        // code after a returning exception remains available to the dispatcher.
        out.insert(ins.pc + 4u);
        break;
    case e_ee_flow::FLOW_NONE:
    default:
        out.insert(ins.pc + 4u);
        break;
    }
    return out;
}


namespace
{
bool is_control_transfer(const s_ee_instruction& ins)
{
    return ins.flow != e_ee_flow::FLOW_NONE;
}

bool is_unconditional(const s_ee_instruction& ins)
{
    return ins.flow == e_ee_flow::FLOW_JUMP ||
           ins.flow == e_ee_flow::FLOW_CALL ||
           ins.flow == e_ee_flow::FLOW_RETURN ||
           ins.flow == e_ee_flow::FLOW_EXCEPTION;
}

bool is_conditional(const s_ee_instruction& ins)
{
    return ins.flow == e_ee_flow::FLOW_BRANCH ||
           ins.flow == e_ee_flow::FLOW_BRANCH_LIKELY;
}

bool is_return(const s_ee_instruction& ins)
{
    return ins.flow == e_ee_flow::FLOW_RETURN;
}
} // namespace

std::vector<s_ee_function> function_analyzer::analyze(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& entry_points,
    const std::vector<uint32_t>& all_entries)
{
    std::set<uint32_t> stop_entries(all_entries.begin(), all_entries.end());

    std::vector<s_ee_function> functions;
    for (uint32_t ep : entry_points)
    {
        s_ee_function fn;
        fn.entry_pc = ep;
        split_into_blocks(insns, ep, fn, stop_entries);
        link_cfg(fn);
        functions.push_back(std::move(fn));
    }
    return functions;
}

void function_analyzer::split_into_blocks(
    const std::map<uint32_t, s_ee_instruction>& insns,
    uint32_t entry_pc,
    s_ee_function& fn,
    const std::set<uint32_t>& stop_entries)
{
    // Another function's entry: do not absorb its body into this CFG.
    auto is_foreign_entry = [&](uint32_t pc) -> bool {
        return pc != entry_pc && stop_entries.count(pc) != 0;
    };

    std::set<uint32_t> visited;
    std::vector<uint32_t> worklist;
    worklist.push_back(entry_pc);

    while (!worklist.empty())
    {
        uint32_t cur = worklist.back();
        worklist.pop_back();
        if (!visited.insert(cur).second) continue;
        // Never start a block at another function's entry.
        if (is_foreign_entry(cur)) continue;

        auto it = insns.find(cur);
        if (it == insns.end()) continue;

        s_ee_basic_block bb;
        bb.start_pc = cur;
        uint32_t pc = cur;

        while (true)
        {
            auto iit = insns.find(pc);
            if (iit == insns.end()) break;

            // Stop *before* consuming a foreign function entry mid-block.
            if (pc != bb.start_pc && is_foreign_entry(pc))
            {
                bb.end_pc = pc;
                bb.fallthrough_pc = pc;
                break;
            }

            const s_ee_instruction& ins = iit->second;
            bb.insn_indices.push_back(pc); // store pc as index key for now

            bb.end_pc = pc + 4;
            bb.fallthrough_pc = pc + 4;

            if (is_control_transfer(ins))
            {
                bb.has_delay_slot = ins.has_delay_slot;
                bb.branch_likely = ins.branch_likely;

                uint32_t delay_pc = pc + 4;

                if (ins.has_delay_slot)
                {
                    // The delay-slot instruction belongs to this block and
                    // executes before the control transfer takes effect.
                    // Do not pull a foreign entry into the delay slot.
                    auto dit = insns.find(delay_pc);
                    if (dit != insns.end() && !is_foreign_entry(delay_pc))
                    {
                        bb.insn_indices.push_back(delay_pc);
                        bb.end_pc = delay_pc + 4;
                        bb.fallthrough_pc = delay_pc + 4;
                    }
                    else
                    {
                        bb.end_pc = delay_pc + 4;
                        bb.fallthrough_pc = delay_pc + 4;
                    }
                }
                else
                {
                    bb.end_pc = pc + 4;
                    bb.fallthrough_pc = pc + 4;
                }

                if (is_conditional(ins))
                {
                    bb.target_pc = ins.branch_target;
                    // Fallthrough: only if it is not another function entry.
                    if (!is_foreign_entry(bb.fallthrough_pc))
                    {
                        bb.successors.insert(bb.fallthrough_pc);
                        worklist.push_back(bb.fallthrough_pc);
                    }
                    // Taken target: follow only if still inside this function.
                    if (!is_foreign_entry(ins.branch_target))
                    {
                        bb.successors.insert(ins.branch_target);
                        worklist.push_back(ins.branch_target);
                    }
                }
                else if (is_unconditional(ins))
                {
                    if (ins.flow == e_ee_flow::FLOW_EXCEPTION)
                    {
                        // Exception entry terminates the guest basic block;
                        // the runtime exception dispatcher chooses the vector.
                        bb.is_terminator = true;
                    }
                    else if (ins.flow == e_ee_flow::FLOW_CALL)
                    {
                        // STOP-AT-CALL: callee body is not part of this CFG.
                        // Only the return path (fallthrough after delay) continues.
                        bb.target_pc = ins.jump_target;
                        if (!is_foreign_entry(bb.fallthrough_pc))
                        {
                            bb.successors.insert(bb.fallthrough_pc);
                            worklist.push_back(bb.fallthrough_pc);
                        }
                        // Intentionally do NOT push jump_target.
                    }
                    else if (is_return(ins))
                    {
                        bb.is_terminator = true;
                    }
                    else
                    {
                        // Pure jump (J / JR-as-jump): follow target if in-function.
                        bb.target_pc = ins.jump_target;
                        if (ins.opcode == e_ee_opcode::OP_J &&
                            !is_foreign_entry(ins.jump_target))
                        {
                            bb.successors.insert(ins.jump_target);
                            worklist.push_back(ins.jump_target);
                        }
                        else
                        {
                            // Tail-call / jump into another function → end.
                            bb.is_terminator = true;
                        }
                    }
                }
                break; // block ends at control transfer (after delay slot)
            }

            bb.end_pc = pc + 4;
            bb.fallthrough_pc = pc + 4;
            pc += 4;

            // About to walk into another function via linear fallthrough.
            if (is_foreign_entry(pc))
                break;
        }

        fn.blocks.push_back(std::move(bb));
    }

    // Order blocks by start_pc for deterministic output.
    std::sort(fn.blocks.begin(), fn.blocks.end(),
        [](const s_ee_basic_block& a, const s_ee_basic_block& b) {
            return a.start_pc < b.start_pc;
        });
}

void function_analyzer::link_cfg(s_ee_function& fn)
{
    // Map pc -> block index for edge linking.
    std::map<uint32_t, size_t> pc_to_block;
    for (size_t i = 0; i < fn.blocks.size(); ++i)
        pc_to_block[fn.blocks[i].start_pc] = i;

    for (auto& bb : fn.blocks)
    {
        for (uint32_t succ : bb.successors)
        {
            auto it = pc_to_block.find(succ);
            if (it != pc_to_block.end())
            {
                fn.blocks[it->second].predecessors.insert(bb.start_pc);
            }
        }
    }
}

} // namespace ee
