#include "ee_dispatcher.h"

namespace ee
{

s_ee_dispatch_result ee_dispatch_run(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    uint32_t max_dispatches)
{
    s_ee_dispatch_result result{};

    if (cpu == nullptr ||
        table.compiled == nullptr ||
        table.entries == nullptr ||
        table.entry_count == 0 ||
        table.entry_count != table.compiled->functions.size() ||
        max_dispatches == 0)
    {
        result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
        if (cpu != nullptr)
        {
            result.exit_reason = cpu->m_exit_reason;
            result.guest_pc = cpu->m_pc;
        }
        return result;
    }

    const s_recompiled_fn* const first_function =
        table.compiled->functions.data();
    const s_recompiled_fn* const end_function =
        first_function + table.compiled->functions.size();

    while (true)
    {
        result.guest_pc = cpu->m_pc;

        if (result.dispatch_count >= max_dispatches)
        {
            result.status = e_ee_dispatch_status::DISPATCH_LIMIT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        const s_recompiled_fn* const owner =
            find_recompiled_function(*table.compiled, cpu->m_pc);

        if (owner == nullptr)
        {
            // The current PC is outside the registered translated code set.
            // Preserve the architectural exit reason for the outer host.
            result.status = e_ee_dispatch_status::EXITED;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        if (owner < first_function || owner >= end_function)
        {
            result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        const size_t function_index =
            static_cast<size_t>(owner - first_function);
        const t_ee_translated_entry entry = table.entries[function_index];

        if (entry == nullptr)
        {
            result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        const uint32_t exit_reason = entry(cpu, runtime);
        cpu->m_exit_reason = exit_reason;

        ++result.dispatch_count;
        result.exit_reason = exit_reason;
        result.guest_pc = cpu->m_pc;

        if (exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN) &&
            exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH))
        {
            result.status = e_ee_dispatch_status::EXITED;
            return result;
        }

        // PC zero is accepted as a start-of-run sentinel only. A guest JR
        // to a null return address must not map back to the ELF entry after
        // translated code has already executed.
        if (cpu->m_pc == 0u)
        {
            result.status = e_ee_dispatch_status::EXITED;
            return result;
        }

        // RETURN can lead back into a translated caller continuation; DISPATCH
        // can lead to another translated function. Resolve both through the
        // same block-PC ownership table on the next iteration.
    }
}

} // namespace ee
