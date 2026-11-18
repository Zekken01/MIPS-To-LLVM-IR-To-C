#pragma once

#include "ee_decoder.h"
#include "ee_issue_timing_probe.h"
#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
// Entirely experimental: zero means the caller has not supplied a latency.
// Supplying numbers does NOT validate them as R5900 hardware timing.
struct s_ee_issue_replay_profile
{
    uint32_t m_integer_latency = 0u;
    uint32_t m_load_latency = 0u;
    // All four fields default to unavailable. A caller must supply timing
    // evidence for each distinct HI/LO operation before it can be replayed.
    uint32_t m_hilo_multiply_latency = 0u;
    uint32_t m_hilo_divide_latency = 0u;
    uint32_t m_hilo_transfer_latency = 0u;
    uint32_t m_hilo_accumulate_latency = 0u;
};

enum class e_ee_issue_replay_status : uint8_t
{
    ISSUED,
    ANNULLED,
    UNCONFIGURED,
    INCOMPLETE_STREAM,
    UNMODELED_INSTRUCTION,
    INVALID_SLOT,
    HALTED
};

// Deterministic offline probe. The input must begin at reference slot 1
// and include every reached instruction and every annulled likely slot.
// This class cannot publish timestamps or install a hardware cycle source.
class c_ee_issue_trace_replay
{
public:
    bool configure(s_ee_issue_replay_profile profile)
    {
        if (profile.m_integer_latency == 0u || profile.m_load_latency == 0u)
        {
            return false;
        }
        m_probe.reset();
        m_profile = profile;
        m_configured = true;
        m_failed = false;
        m_next_slot = 1u;
        m_next_attempt = 1u;
        m_pending_likely_slot = false;
        m_delay_pc = 0u;
        m_failure_pc = 0u;
        return true;
    }

    e_ee_issue_replay_status observe(const s_ee_instruction_trace_entry& entry)
    {
        if (!m_configured)
        {
            return e_ee_issue_replay_status::UNCONFIGURED;
        }
        if (m_failed)
        {
            return e_ee_issue_replay_status::HALTED;
        }
        if (entry.m_reference_slot != m_next_slot)
        {
            return fail(e_ee_issue_replay_status::INCOMPLETE_STREAM, entry.m_pc);
        }
        if (m_pending_likely_slot && entry.m_pc != m_delay_pc)
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }

        if (entry.m_kind == 1u)
        {
            if (!m_pending_likely_slot || entry.m_attempt != m_next_attempt - 1u ||
                !m_probe.reserve_annulled_delay_slot())
            {
                return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
            }
            m_pending_likely_slot = false;
            ++m_next_slot;
            return e_ee_issue_replay_status::ANNULLED;
        }

        if ((entry.m_kind != 0u && entry.m_kind != 2u) ||
            entry.m_attempt != m_next_attempt)
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }

        const s_ee_instruction decoded = decode(entry.m_pc, entry.m_opcode_word);
        s_ee_probe_instruction instruction{};
        if (decoded.valid != e_ee_valid::VALID ||
            !classify(decoded, &instruction))
        {
            return fail(e_ee_issue_replay_status::UNMODELED_INSTRUCTION, entry.m_pc);
        }
        if (!m_probe.issue(instruction))
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }

        m_pending_likely_slot = decoded.branch_likely;
        m_delay_pc = decoded.pc + 4u;
        ++m_next_slot;
        ++m_next_attempt;
        return e_ee_issue_replay_status::ISSUED;
    }

    bool usable_prefix() const { return m_configured && !m_failed; }
    uint64_t candidate_cycles() const { return m_probe.estimated_cycles(); }
    uint64_t executed_instructions() const { return m_probe.instructions_issued(); }
    uint64_t annulled_slots() const { return m_probe.annulled_slots_reserved(); }
    uint32_t failure_pc() const { return m_failure_pc; }

private:
    static uint32_t mask(uint32_t register_index)
    {
        return register_index == 0u ? 0u : 1u << register_index;
    }

    bool classify(const s_ee_instruction& decoded,
                  s_ee_probe_instruction* instruction) const
    {
        const uint32_t op = decoded.opcode_word >> 26u;
        const uint32_t rs = mask(decoded.rs);
        const uint32_t rt = mask(decoded.rt);
        const uint32_t rd = mask(decoded.rd);
        instruction->m_result_latency = m_profile.m_integer_latency;
        instruction->m_pipe = e_ee_probe_pipe::INTEGER;

        if (op == 0u)
        {
            switch (decoded.funct)
            {
            case 0x00u: case 0x02u: case 0x03u:
            case 0x38u: case 0x3au: case 0x3bu:
            case 0x3cu: case 0x3eu: case 0x3fu:
                instruction->m_gpr_read_mask = rt;
                instruction->m_gpr_write_mask = rd;
                return true;
            case 0x04u: case 0x06u: case 0x07u:
            case 0x14u: case 0x16u: case 0x17u:
            case 0x0au: case 0x0bu:
            case 0x21u: case 0x23u: case 0x24u:
            case 0x25u: case 0x26u: case 0x27u:
            case 0x2au: case 0x2bu: case 0x2du: case 0x2fu:
                instruction->m_gpr_read_mask = rs | rt;
                instruction->m_gpr_write_mask = rd;
                return true;
            case 0x10u: case 0x12u: // MFHI/MFLO (pipeline 0)
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
                instruction->m_hilo_read_mask =
                    decoded.funct == 0x10u ? 0x1u : 0x2u;
                instruction->m_gpr_write_mask = rd;
                return true;
            case 0x11u: case 0x13u: // MTHI/MTLO (pipeline 0)
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
                instruction->m_gpr_read_mask = rs;
                instruction->m_hilo_write_mask =
                    decoded.funct == 0x11u ? 0x1u : 0x2u;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x18u: case 0x19u: // MULT/MULTU
            case 0x1au: case 0x1bu: // DIV/DIVU
            {
                const bool multiply = decoded.funct < 0x1au;
                const uint32_t special_latency = multiply
                    ? m_profile.m_hilo_multiply_latency
                    : m_profile.m_hilo_divide_latency;
                if (special_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
                instruction->m_gpr_read_mask = rs | rt;
                // EE MULT can also put the low product directly in rd.
                instruction->m_gpr_write_mask = multiply ? rd : 0u;
                instruction->m_hilo_write_mask = 0x3u;
                instruction->m_hilo_result_latency = special_latency;
                return true;
            }
            case 0x08u: case 0x09u:
                instruction->m_pipe = e_ee_probe_pipe::BRANCH;
                instruction->m_gpr_read_mask = rs;
                instruction->m_gpr_write_mask =
                    decoded.funct == 0x09u ? rd : 0u;
                return true;
            default:
                return false; // HI/LO, trap, syscall and synchronization
            }
        }

        // MMI HI1/LO1 and multiply/accumulate have independent
        // architectural hazards from the scalar HI0/LO0 pipeline.
        if (op == 0x1cu)
        {
            const uint32_t operation = decoded.funct;
            instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
            switch (operation)
            {
            case 0x10u: case 0x12u: // MFHI1 / MFLO1
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_hilo_read_mask =
                    operation == 0x10u ? 0x4u : 0x8u;
                instruction->m_gpr_write_mask = rd;
                return true;
            case 0x11u: case 0x13u: // MTHI1 / MTLO1
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rs;
                instruction->m_hilo_write_mask =
                    operation == 0x11u ? 0x4u : 0x8u;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x18u: case 0x19u: // MULT1 / MULTU1
            case 0x1au: case 0x1bu: // DIV1 / DIVU1
            {
                const bool multiply = operation < 0x1au;
                const uint32_t special_latency = multiply
                    ? m_profile.m_hilo_multiply_latency
                    : m_profile.m_hilo_divide_latency;
                if (special_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rs | rt;
                instruction->m_gpr_write_mask = multiply ? rd : 0u;
                instruction->m_hilo_write_mask = 0xcu;
                instruction->m_hilo_result_latency = special_latency;
                return true;
            }
            case 0x00u: case 0x01u: // MADD / MADDU
            case 0x20u: case 0x21u: // MADD1 / MADDU1
                if (m_profile.m_hilo_accumulate_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rs | rt;
                instruction->m_gpr_write_mask = rd;
                instruction->m_hilo_read_mask =
                    operation < 0x20u ? 0x3u : 0xcu;
                instruction->m_hilo_write_mask =
                    instruction->m_hilo_read_mask;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_accumulate_latency;
                return true;
            default:
                // Packed MMI still requires vector-lane scoreboard modeling.
                return false;
            }
        }

        switch (op)
        {
        case 0x02u: case 0x03u:
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_write_mask = op == 0x03u ? mask(31u) : 0u;
            return true;
        case 0x01u:
            if (decoded.rt != 0u && decoded.rt != 1u &&
                decoded.rt != 2u && decoded.rt != 3u &&
                decoded.rt != 16u && decoded.rt != 17u &&
                decoded.rt != 18u && decoded.rt != 19u)
            {
                return false;
            }
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_read_mask = rs;
            instruction->m_gpr_write_mask =
                decoded.rt >= 16u ? mask(31u) : 0u;
            return true;
        case 0x04u: case 0x05u: case 0x14u: case 0x15u:
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_read_mask = rs | rt;
            return true;
        case 0x06u: case 0x07u: case 0x16u: case 0x17u:
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_read_mask = rs;
            return true;
        case 0x09u: case 0x0au: case 0x0bu: case 0x0cu:
        case 0x0du: case 0x0eu: case 0x19u:
            instruction->m_gpr_read_mask = rs;
            instruction->m_gpr_write_mask = rt;
            return true;
        case 0x0fu:
            instruction->m_gpr_write_mask = rt;
            return true;
        case 0x20u: case 0x21u: case 0x23u:
        case 0x24u: case 0x25u: case 0x27u: case 0x37u:
            instruction->m_pipe = e_ee_probe_pipe::LOAD;
            instruction->m_result_latency = m_profile.m_load_latency;
            instruction->m_gpr_read_mask = rs;
            instruction->m_gpr_write_mask = rt;
            return true;
        case 0x28u: case 0x29u: case 0x2bu: case 0x3fu:
            instruction->m_pipe = e_ee_probe_pipe::STORE;
            instruction->m_gpr_read_mask = rs | rt;
            return true;
        default:
            return false; // Unknown pipeline or hazard behaviour
        }
    }

    e_ee_issue_replay_status fail(e_ee_issue_replay_status code, uint32_t pc)
    {
        m_failed = true;
        m_failure_pc = pc;
        return code;
    }

    c_ee_issue_timing_probe m_probe{};
    s_ee_issue_replay_profile m_profile{};
    bool m_configured = false;
    bool m_failed = false;
    bool m_pending_likely_slot = false;
    uint32_t m_delay_pc = 0u;
    uint32_t m_failure_pc = 0u;
    uint64_t m_next_slot = 1u;
    uint64_t m_next_attempt = 1u;
};

} // namespace ee
