#include "function_analyzer.h"
#include "ee_opcode.h"
#include <algorithm>
#include <array>
#include <deque>
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
struct s_gpr_constant_state
{
    bool m_reachable = false;
    std::array<uint64_t, 32> m_value{};
    std::array<bool, 32> m_known{};
};

void set_gpr_constant(s_gpr_constant_state& state, uint32_t reg, uint64_t value)
{
    if (reg == 0 || reg >= 32)
    {
        return;
    }

    state.m_value[reg] = value;
    state.m_known[reg] = true;
}

void clear_gpr_constant(s_gpr_constant_state& state, uint32_t reg)
{
    if (reg == 0 || reg >= 32)
    {
        return;
    }

    state.m_known[reg] = false;
}

void clear_all_gpr_constants(s_gpr_constant_state& state)
{
    state.m_known.fill(false);
    state.m_known[0] = true;
    state.m_value[0] = 0;
}

bool read_gpr_constant(
    const s_gpr_constant_state& state,
    uint32_t reg,
    uint64_t* value
)
{
    if (!value || reg >= 32)
    {
        return false;
    }

    if (reg == 0)
    {
        *value = 0;
        return true;
    }

    if (!state.m_known[reg])
    {
        return false;
    }

    *value = state.m_value[reg];
    return true;
}

uint64_t sign_extend_u32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(
        static_cast<int32_t>(value)
    ));
}

bool merge_gpr_state(
    s_gpr_constant_state& destination,
    const s_gpr_constant_state& source
)
{
    if (!source.m_reachable)
    {
        return false;
    }

    if (!destination.m_reachable)
    {
        destination = source;
        return true;
    }

    bool changed = false;
    for (uint32_t reg = 1; reg < 32; ++reg)
    {
        if (destination.m_known[reg] &&
            (!source.m_known[reg] ||
             destination.m_value[reg] != source.m_value[reg]))
        {
            destination.m_known[reg] = false;
            changed = true;
        }
    }

    return changed;
}

bool is_special_no_gpr_write(uint8_t funct)
{
    switch (funct)
    {
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_JR):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_SYSCALL):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_BREAK):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_SYNC):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MTHI):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MTLO):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MULT):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MULTU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_DIV):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_DIVU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TGE):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TGEU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TLT):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TLTU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TEQ):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TNE):
        return true;
    default:
        return false;
    }
}

void invalidate_unmodelled_destination(
    const s_ee_instruction& ins,
    s_gpr_constant_state& state
)
{
    switch (ins.opcode)
    {
    case e_ee_opcode::OP_SPECIAL:
        if (!is_special_no_gpr_write(ins.funct))
        {
            clear_gpr_constant(state, ins.rd);
        }
        return;

    case e_ee_opcode::OP_REGIMM:
        if (ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZAL) ||
            ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZAL) ||
            ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZALL) ||
            ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZALL))
        {
            clear_gpr_constant(state, 31);
        }
        return;

    case e_ee_opcode::OP_JAL:
        set_gpr_constant(state, 31, sign_extend_u32(ins.pc + 8u));
        return;

    case e_ee_opcode::OP_ADDI:
    case e_ee_opcode::OP_SLTI:
    case e_ee_opcode::OP_SLTIU:
    case e_ee_opcode::OP_ANDI:
    case e_ee_opcode::OP_XORI:
    case e_ee_opcode::OP_LUI:
    case e_ee_opcode::OP_DADDI:
    case e_ee_opcode::OP_DADDIU:
    case e_ee_opcode::OP_LDL:
    case e_ee_opcode::OP_LDR:
    case e_ee_opcode::OP_LQ:
    case e_ee_opcode::OP_LB:
    case e_ee_opcode::OP_LH:
    case e_ee_opcode::OP_LWL:
    case e_ee_opcode::OP_LW:
    case e_ee_opcode::OP_LBU:
    case e_ee_opcode::OP_LHU:
    case e_ee_opcode::OP_LWR:
    case e_ee_opcode::OP_LWU:
    case e_ee_opcode::OP_LD:
        clear_gpr_constant(state, ins.rt);
        return;

    case e_ee_opcode::OP_COP0:
        if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP0_MFC0))
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_COP1:
        if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_MFC1) ||
            ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_CFC1))
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_COP2:
        if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_QMFC2) ||
            ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_CFC2))
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_MMI:
        // MMI subfamilies use different destination-field conventions.
        // Killing both candidates is conservative and avoids false targets.
        clear_gpr_constant(state, ins.rd);
        clear_gpr_constant(state, ins.rt);
        return;

    default:
        break;
    }

    if (ins.valid != e_ee_valid::VALID)
    {
        // Unknown/invalid semantics may modify any GPR.
        clear_all_gpr_constants(state);
    }
}

void transfer_gpr_constants(
    const s_ee_instruction& ins,
    s_gpr_constant_state& state
)
{
    uint64_t source_s = 0;
    uint64_t source_t = 0;
    bool has_s = read_gpr_constant(state, ins.rs, &source_s);
    bool has_t = read_gpr_constant(state, ins.rt, &source_t);
    const uint32_t imm32 = static_cast<uint32_t>(
        static_cast<int32_t>(ins.immediate)
    );

    switch (ins.opcode)
    {
    case e_ee_opcode::OP_LUI:
        set_gpr_constant(
            state, ins.rt,
            sign_extend_u32(static_cast<uint32_t>(ins.uimmediate) << 16)
        );
        return;

    case e_ee_opcode::OP_ORI:
        if (has_s)
        {
            set_gpr_constant(state, ins.rt, source_s | ins.uimmediate);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_ANDI:
        if (has_s)
        {
            set_gpr_constant(state, ins.rt, source_s & ins.uimmediate);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_XORI:
        if (has_s)
        {
            set_gpr_constant(state, ins.rt, source_s ^ ins.uimmediate);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_ADDIU:
        if (has_s)
        {
            const uint32_t result =
                static_cast<uint32_t>(source_s) + imm32;
            set_gpr_constant(state, ins.rt, sign_extend_u32(result));
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_DADDIU:
        if (has_s)
        {
            set_gpr_constant(
                state, ins.rt,
                source_s + static_cast<uint64_t>(
                    static_cast<int64_t>(ins.immediate)
                )
            );
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_SPECIAL:
        break;

    default:
        invalidate_unmodelled_destination(ins, state);
        return;
    }

    using op = e_ee_opcode;
    switch (ins.funct)
    {
    case static_cast<uint8_t>(op::FUNCT_ADDU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd,
                sign_extend_u32(static_cast<uint32_t>(source_s + source_t)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DADDU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s + source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SUBU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd,
                sign_extend_u32(static_cast<uint32_t>(source_s - source_t)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSUBU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s - source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_AND):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s & source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_OR):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s | source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_XOR):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s ^ source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_NOR):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, ~(source_s | source_t));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SLL):
        if (has_t)
            set_gpr_constant(state, ins.rd, sign_extend_u32(
                static_cast<uint32_t>(source_t) << ins.sa));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SRL):
        if (has_t)
            set_gpr_constant(state, ins.rd, sign_extend_u32(
                static_cast<uint32_t>(source_t) >> ins.sa));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SRA):
        if (has_t)
            set_gpr_constant(state, ins.rd, sign_extend_u32(
                static_cast<uint32_t>(
                    static_cast<int32_t>(source_t) >> ins.sa)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSLL):
    case static_cast<uint8_t>(op::FUNCT_DSLL32):
        if (has_t)
            set_gpr_constant(state, ins.rd,
                source_t << (ins.sa + (ins.funct == static_cast<uint8_t>(op::FUNCT_DSLL32) ? 32u : 0u)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRL):
    case static_cast<uint8_t>(op::FUNCT_DSRL32):
        if (has_t)
            set_gpr_constant(state, ins.rd,
                source_t >> (ins.sa + (ins.funct == static_cast<uint8_t>(op::FUNCT_DSRL32) ? 32u : 0u)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRA):
    case static_cast<uint8_t>(op::FUNCT_DSRA32):
        if (has_t)
            set_gpr_constant(state, ins.rd, static_cast<uint64_t>(
                static_cast<int64_t>(source_t) >>
                (ins.sa + (ins.funct == static_cast<uint8_t>(op::FUNCT_DSRA32) ? 32u : 0u))));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSLLV):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_t << (source_s & 0x3fu));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRLV):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_t >> (source_s & 0x3fu));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRAV):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, static_cast<uint64_t>(
                static_cast<int64_t>(source_t) >> (source_s & 0x3fu)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_JALR):
        set_gpr_constant(state, ins.rd, sign_extend_u32(ins.pc + 8u));
        return;

    default:
        invalidate_unmodelled_destination(ins, state);
        return;
    }
}

void transfer_block_constants(
    const s_ee_basic_block& block,
    const std::map<uint32_t, s_ee_instruction>& insns,
    s_gpr_constant_state& state,
    size_t instruction_count = static_cast<size_t>(-1)
)
{
    const size_t count = std::min(instruction_count, block.insn_indices.size());
    for (size_t i = 0; i < count; ++i)
    {
        const size_t pc_index = block.insn_indices[i];
        auto it = insns.find(static_cast<uint32_t>(pc_index));
        if (it == insns.end())
        {
            clear_all_gpr_constants(state);
            continue;
        }

        transfer_gpr_constants(it->second, state);
    }

    state.m_known[0] = true;
    state.m_value[0] = 0;
}

std::set<uint32_t> collect_jalr_targets_in_block(
    const s_ee_basic_block& block,
    const std::map<uint32_t, s_ee_instruction>& insns,
    s_gpr_constant_state state
)
{
    std::set<uint32_t> targets;
    for (size_t pc_index : block.insn_indices)
    {
        auto it = insns.find(static_cast<uint32_t>(pc_index));
        if (it == insns.end())
        {
            clear_all_gpr_constants(state);
            continue;
        }

        const s_ee_instruction& ins = it->second;
        if (ins.valid == e_ee_valid::VALID &&
            ins.opcode == e_ee_opcode::OP_SPECIAL &&
            ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_JALR))
        {
            uint64_t target_value = 0;
            if (read_gpr_constant(state, ins.rs, &target_value))
            {
                const uint32_t target = static_cast<uint32_t>(target_value);
                if (target != 0 && (target & 3u) == 0)
                {
                    targets.insert(target);
                }
            }
        }

        transfer_gpr_constants(ins, state);
    }
    return targets;
}

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

std::set<uint32_t> collect_static_jalr_targets(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& entry_points,
    const std::vector<uint32_t>& all_entries
)
{
    std::set<uint32_t> targets;
    function_analyzer analyzer;

    auto states_equal = [](const s_gpr_constant_state& a,
                           const s_gpr_constant_state& b) -> bool
    {
        return a.m_reachable == b.m_reachable &&
               a.m_known == b.m_known &&
               a.m_value == b.m_value;
    };

    for (uint32_t entry : entry_points)
    {
        const std::vector<s_ee_function> functions =
            analyzer.analyze(insns, {entry}, all_entries);
        if (functions.empty() || functions[0].blocks.empty())
        {
            continue;
        }

        const s_ee_function& fn = functions[0];
        std::map<uint32_t, const s_ee_basic_block*> blocks;
        for (const auto& block : fn.blocks)
        {
            blocks[block.start_pc] = &block;
        }

        if (blocks.find(entry) == blocks.end())
        {
            continue;
        }

        std::map<uint32_t, s_gpr_constant_state> incoming;
        std::map<uint32_t, s_gpr_constant_state> outgoing;
        std::deque<uint32_t> worklist;
        std::set<uint32_t> queued;

        s_gpr_constant_state entry_state{};
        entry_state.m_reachable = true;
        entry_state.m_known[0] = true;
        entry_state.m_value[0] = 0;
        incoming[entry] = entry_state;
        worklist.push_back(entry);
        queued.insert(entry);

        // Forward data-flow over the function CFG. The lattice only moves
        // from a known constant to unknown at a join, so this converges even
        // in the presence of loops and recursive-looking local control flow.
        while (!worklist.empty())
        {
            const uint32_t block_pc = worklist.front();
            worklist.pop_front();
            queued.erase(block_pc);

            const auto block_it = blocks.find(block_pc);
            const auto in_it = incoming.find(block_pc);
            if (block_it == blocks.end() ||
                in_it == incoming.end() ||
                !in_it->second.m_reachable)
            {
                continue;
            }

            const s_ee_basic_block& block = *block_it->second;
            s_gpr_constant_state out_state = in_it->second;
            transfer_block_constants(block, insns, out_state);
            out_state.m_reachable = true;

            const bool annul_delay_on_fallthrough =
                block.branch_likely && block.has_delay_slot &&
                block.insn_indices.size() > 1;
            s_gpr_constant_state out_fallthrough = out_state;
            if (annul_delay_on_fallthrough)
            {
                out_fallthrough = in_it->second;
                // The final listed instruction is the delay slot. It executes
                // only on the taken path for a branch-likely instruction.
                transfer_block_constants(
                    block, insns, out_fallthrough,
                    block.insn_indices.size() - 1u
                );
                out_fallthrough.m_reachable = true;
            }

            const auto old_out = outgoing.find(block_pc);
            if (old_out != outgoing.end() &&
                states_equal(old_out->second, out_state))
            {
                // The taken-edge state is unchanged. The annulled fallthrough
                // state is a deterministic projection of the same input, so
                // no successor needs to be revisited either.
                continue;
            }
            outgoing[block_pc] = out_state;

            for (uint32_t successor : block.successors)
            {
                if (blocks.find(successor) == blocks.end())
                {
                    continue;
                }

                bool successor_changed = false;
                const bool is_fallthrough =
                    annul_delay_on_fallthrough &&
                    successor == block.fallthrough_pc;
                const bool is_taken_target =
                    annul_delay_on_fallthrough &&
                    successor == block.target_pc;

                if (is_fallthrough)
                {
                    successor_changed |= merge_gpr_state(
                        incoming[successor], out_fallthrough
                    );
                }

                if (!is_fallthrough || is_taken_target)
                {
                    successor_changed |= merge_gpr_state(
                        incoming[successor], out_state
                    );
                }

                if (successor_changed && queued.insert(successor).second)
                {
                    worklist.push_back(successor);
                }
            }
        }

        // Collect only after the data-flow fixpoint. This avoids reporting a
        // target seen on one path before a later predecessor disproves it.
        for (const auto& block : fn.blocks)
        {
            const auto in_it = incoming.find(block.start_pc);
            if (in_it == incoming.end() || !in_it->second.m_reachable)
            {
                continue;
            }

            const std::set<uint32_t> block_targets =
                collect_jalr_targets_in_block(block, insns, in_it->second);
            targets.insert(block_targets.begin(), block_targets.end());
        }
    }

    return targets;
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

            // A branch/jump target may already have been scheduled and
            // materialised as its own basic block. Do not absorb that target
            // into a later linear scan: terminate here and preserve the CFG
            // edge so predecessor data-flow and block emission stay sound.
            if (pc != bb.start_pc && visited.count(pc) != 0)
            {
                bb.end_pc = pc;
                bb.fallthrough_pc = pc;
                bb.successors.insert(pc);
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
                        // The exception handler chooses the vector and the
                        // current translated block exits to the host. It may
                        // later resume at EPC+4, so retain that PC as a
                        // separately dispatchable continuation block.
                        bb.is_terminator = true;
                        if (!is_foreign_entry(bb.fallthrough_pc))
                        {
                            bb.successors.insert(bb.fallthrough_pc);
                            worklist.push_back(bb.fallthrough_pc);
                        }
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
