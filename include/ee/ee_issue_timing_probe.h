#pragma once

#include <cstdint>
#include <limits>

namespace ee
{

// Explicitly experimental issue-timing probe. This is NOT a calibrated
// R5900 clock and MUST NOT be connected to GS, INTC, DMA or guest timers.
// Inputs describe a committed guest instruction, not a host dispatch.
enum class e_ee_probe_pipe : uint8_t
{
    INTEGER,
    LOAD,
    STORE,
    BRANCH,
    MULT_DIV,
    COPROCESSOR
};

struct s_ee_probe_instruction
{
    uint32_t m_gpr_read_mask = 0u;
    uint32_t m_gpr_write_mask = 0u;
    e_ee_probe_pipe m_pipe = e_ee_probe_pipe::INTEGER;
    // Experimental scoreboard reservation, supplied by a validated caller.
    // A latency of one allows a dependent instruction in the next probe cycle.
    uint32_t m_result_latency = 1u;

    // Resource identity is architectural; result availability is supplied by
    // the caller, never inferred from an unverified latency table.
    // HI/LO mask: bit0=HI0 bit1=LO0 bit2=HI1 bit3=LO1.
    uint32_t m_hilo_read_mask = 0u;
    uint32_t m_hilo_write_mask = 0u;
    uint32_t m_fpr_read_mask = 0u;
    uint32_t m_fpr_write_mask = 0u;
    bool m_acc_read = false;
    bool m_acc_write = false;
    // FCR31 status/condition bits share an architectural control register.
    // This is a single conservative resource, not independently timed FCC bits.
    bool m_fcsr_read = false;
    bool m_fcsr_write = false;

    // Zero falls back to m_result_latency. Multiplication, division and
    // FPU operations may produce GPR and special-register results at
    // different times; caller must supply those when modeling such opcodes.
    uint32_t m_hilo_result_latency = 0u;
    uint32_t m_fpr_result_latency = 0u;
    uint32_t m_acc_result_latency = 0u;
    uint32_t m_fcsr_result_latency = 0u;
};

// A conservative, deterministic two-wide issue *candidate* for evaluating
// instruction streams. Non-integer operations consume a cycle alone.
// Pairing also rejects RAW/WAR/WAW register overlap. The caller supplies
// result latency explicitly; there are no invented hardware latency tables.
// It deliberately cannot change CPU or device state.
class c_ee_issue_timing_probe
{
public:
    void reset()
    {
        m_cycle = 0u;
        m_issued = 0u;
        m_annulled = 0u;
        m_hle_overhead = 0u;
        m_pair_count = 0u;
        clear_pair();
        for (uint64_t& ready : m_gpr_ready)
        {
            ready = 0u;
        }
        for (uint64_t& ready : m_hilo_ready)
        {
            ready = 0u;
        }
        for (uint64_t& ready : m_fpr_ready)
        {
            ready = 0u;
        }
        m_acc_ready = 0u;
        m_fcsr_ready = 0u;
    }

    // Returns false without mutation on overflow or invalid input.
    bool issue(const s_ee_probe_instruction& instruction,
               uint64_t* issue_cycle = nullptr)
    {
        if (instruction.m_result_latency == 0u ||
            ((instruction.m_hilo_read_mask | instruction.m_hilo_write_mask) &
                ~0x0fu) != 0u ||
            m_issued == std::numeric_limits<uint64_t>::max())
        {
            return false;
        }

        const uint32_t reads = instruction.m_gpr_read_mask & ~1u;
        const uint32_t writes = instruction.m_gpr_write_mask & ~1u;
        uint64_t candidate = m_cycle;

        const uint32_t hreads = instruction.m_hilo_read_mask;
        const uint32_t hwrites = instruction.m_hilo_write_mask;
        const uint32_t freads = instruction.m_fpr_read_mask;
        const uint32_t fwrites = instruction.m_fpr_write_mask;
        const bool independent =
            (reads & m_pair_writes) == 0u &&
            (writes & (m_pair_reads | m_pair_writes)) == 0u &&
            (hreads & m_pair_hilo_writes) == 0u &&
            (hwrites & (m_pair_hilo_reads | m_pair_hilo_writes)) == 0u &&
            (freads & m_pair_fpr_writes) == 0u &&
            (fwrites & (m_pair_fpr_reads | m_pair_fpr_writes)) == 0u &&
            (!instruction.m_acc_read || !m_pair_acc_write) &&
            (!instruction.m_acc_write ||
                (!m_pair_acc_read && !m_pair_acc_write)) &&
            (!instruction.m_fcsr_read || !m_pair_fcsr_write) &&
            (!instruction.m_fcsr_write ||
                (!m_pair_fcsr_read && !m_pair_fcsr_write));
        const bool may_pair =
            m_pair_count == 1u && m_pair_integer &&
            instruction.m_pipe == e_ee_probe_pipe::INTEGER && independent;

        if (m_pair_count != 0u && !may_pair)
        {
            if (candidate == std::numeric_limits<uint64_t>::max())
            {
                return false;
            }
            ++candidate;
        }

        for (uint32_t reg = 1u; reg < 32u; ++reg)
        {
            if ((reads & (uint32_t{1} << reg)) != 0u &&
                candidate < m_gpr_ready[reg])
            {
                candidate = m_gpr_ready[reg];
            }
        }

        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            if ((hreads & (1u << reg)) != 0u &&
                candidate < m_hilo_ready[reg])
            {
                candidate = m_hilo_ready[reg];
            }
        }
        for (uint32_t reg = 0u; reg < 32u; ++reg)
        {
            if ((freads & (1u << reg)) != 0u &&
                candidate < m_fpr_ready[reg])
            {
                candidate = m_fpr_ready[reg];
            }
        }
        if (instruction.m_acc_read && candidate < m_acc_ready)
        {
            candidate = m_acc_ready;
        }
        if (instruction.m_fcsr_read && candidate < m_fcsr_ready)
        {
            candidate = m_fcsr_ready;
        }

        const uint32_t hilo_latency = instruction.m_hilo_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_hilo_result_latency;
        const uint32_t fpr_latency = instruction.m_fpr_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_fpr_result_latency;
        const uint32_t acc_latency = instruction.m_acc_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_acc_result_latency;
        const uint32_t fcsr_latency = instruction.m_fcsr_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_fcsr_result_latency;

        // A failed issue cannot change the scoreboard. Validate ALL result
        // domains before mutating any state or accepting a new issue.
        const uint64_t max_cycle = std::numeric_limits<uint64_t>::max();
        if (candidate >= max_cycle ||
            instruction.m_result_latency > max_cycle - candidate ||
            (hwrites != 0u && hilo_latency > max_cycle - candidate) ||
            (fwrites != 0u && fpr_latency > max_cycle - candidate) ||
            (instruction.m_acc_write &&
                acc_latency > max_cycle - candidate) ||
            (instruction.m_fcsr_write &&
                fcsr_latency > max_cycle - candidate))
        {
            return false;
        }

        if (candidate != m_cycle)
        {
            m_cycle = candidate;
            clear_pair();
        }

        m_pair_reads |= reads;
        m_pair_writes |= writes;
        m_pair_hilo_reads |= hreads;
        m_pair_hilo_writes |= hwrites;
        m_pair_fpr_reads |= freads;
        m_pair_fpr_writes |= fwrites;
        m_pair_acc_read |= instruction.m_acc_read;
        m_pair_acc_write |= instruction.m_acc_write;
        m_pair_fcsr_read |= instruction.m_fcsr_read;
        m_pair_fcsr_write |= instruction.m_fcsr_write;
        m_pair_integer = instruction.m_pipe == e_ee_probe_pipe::INTEGER;
        ++m_pair_count;
        for (uint32_t reg = 1u; reg < 32u; ++reg)
        {
            if ((writes & (uint32_t{1} << reg)) != 0u)
            {
                m_gpr_ready[reg] =
                    candidate + instruction.m_result_latency;
            }
        }
        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            if ((hwrites & (1u << reg)) != 0u)
            {
                m_hilo_ready[reg] = candidate + hilo_latency;
            }
        }
        for (uint32_t reg = 0u; reg < 32u; ++reg)
        {
            if ((fwrites & (1u << reg)) != 0u)
            {
                m_fpr_ready[reg] = candidate + fpr_latency;
            }
        }
        if (instruction.m_acc_write)
        {
            m_acc_ready = candidate + acc_latency;
        }
        if (instruction.m_fcsr_write)
        {
            m_fcsr_ready = candidate + fcsr_latency;
        }
        ++m_issued;
        if (issue_cycle != nullptr)
        {
            *issue_cycle = candidate;
        }
        return true;
    }

    // Not-taken branch-likely: count a reserved slot without executing its
    // instruction or creating a guest side effect. A previous branch alone
    // already occupied its issue cycle.
    bool reserve_annulled_delay_slot()
    {
        const uint64_t max_cycle = std::numeric_limits<uint64_t>::max();
        if (m_annulled == max_cycle ||
            m_cycle >= max_cycle - 1u)
        {
            return false;
        }
        if (m_pair_count != 0u)
        {
            ++m_cycle;
        }
        clear_pair();
        m_pair_count = 1u;
        ++m_annulled;
        return true;
    }

    // Explicit caller-configured HLE cost, separate from issue slots.
    // For example the reference runtime defaults to 300, but this number
    // has NOT been validated as an R5900 syscall latency.
    bool charge_hle_overhead(uint32_t cycles)
    {
        const uint64_t max_cycle = std::numeric_limits<uint64_t>::max();
        const uint64_t finish = m_cycle + (m_pair_count != 0u ? 1u : 0u);
        if (finish < m_cycle || cycles > max_cycle - finish ||
            cycles > max_cycle - m_hle_overhead)
        {
            return false;
        }
        m_cycle = finish + cycles;
        clear_pair();
        m_hle_overhead += cycles;
        return true;
    }

    uint64_t estimated_cycles() const
    {
        return m_cycle + (m_pair_count != 0u ? 1u : 0u);
    }

    uint64_t instructions_issued() const
    {
        return m_issued;
    }

    uint64_t annulled_slots_reserved() const
    {
        return m_annulled;
    }

    uint64_t configured_hle_overhead() const
    {
        return m_hle_overhead;
    }

    // A diagnostic boundary check only. It never fires a hardware event.
    bool probe_budget_reached(uint64_t budget) const
    {
        return budget != 0u && estimated_cycles() >= budget;
    }

private:
    void clear_pair()
    {
        m_pair_count = 0u;
        m_pair_reads = 0u;
        m_pair_writes = 0u;
        m_pair_hilo_reads = 0u;
        m_pair_hilo_writes = 0u;
        m_pair_fpr_reads = 0u;
        m_pair_fpr_writes = 0u;
        m_pair_acc_read = false;
        m_pair_acc_write = false;
        m_pair_fcsr_read = false;
        m_pair_fcsr_write = false;
        m_pair_integer = false;
    }

    uint64_t m_cycle = 0u;
    uint64_t m_issued = 0u;
    uint64_t m_annulled = 0u;
    uint64_t m_hle_overhead = 0u;
    uint64_t m_gpr_ready[32]{};
    uint64_t m_hilo_ready[4]{};
    uint64_t m_fpr_ready[32]{};
    uint64_t m_acc_ready = 0u;
    uint64_t m_fcsr_ready = 0u;
    uint32_t m_pair_count = 0u;
    uint32_t m_pair_reads = 0u;
    uint32_t m_pair_writes = 0u;
    uint32_t m_pair_hilo_reads = 0u;
    uint32_t m_pair_hilo_writes = 0u;
    uint32_t m_pair_fpr_reads = 0u;
    uint32_t m_pair_fpr_writes = 0u;
    bool m_pair_acc_read = false;
    bool m_pair_acc_write = false;
    bool m_pair_fcsr_read = false;
    bool m_pair_fcsr_write = false;
    bool m_pair_integer = false;
};

} // namespace ee
