#pragma once

#include "ee_recomp.h"

#include <cstddef>
#include <cstdint>

namespace ee
{

// Native entry point produced by the eventual LLVM object backend.
using t_ee_translated_entry = uint32_t (*)(s_ee_cpu_state*, s_ee_runtime*);

// Runtime code pointers are kept parallel to s_recompiled_elf::functions.
// This keeps the static guest-PC metadata separate from the host code address.
struct s_ee_dispatch_table
{
    const s_recompiled_elf* compiled = nullptr;
    const t_ee_translated_entry* entries = nullptr;
    size_t entry_count = 0;
};

enum class e_ee_dispatch_status : uint8_t
{
    EXITED,
    DISPATCH_LIMIT,
    INVALID_ARGUMENT
};

struct s_ee_dispatch_result
{
    e_ee_dispatch_status status = e_ee_dispatch_status::INVALID_ARGUMENT;
    uint32_t exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
    uint32_t dispatch_count = 0;
    uint32_t guest_pc = 0;
};

// Run translated functions until execution exits the registered code set,
// an architectural exit other than RETURN/DISPATCH occurs, or the dispatch
// budget is exhausted. A pending and enabled COP0 interrupt is sampled before
// each mapped translated-function entry (except the synthetic PC-zero sentinel).
// Interrupt entry sets EPC, Status.EXL, Cause and the exception vector PC,
// returning EXITED + EE_EXIT_EXCEPTION without invoking the next function.
// This is a *function-boundary* checkpoint; long-running translated functions
// cannot be preempted until they return to this dispatcher.
// RETURN and DISPATCH both re-enter translated code when
// cpu->m_pc belongs to a registered function; otherwise the exit is returned
// to the outer host (which may handle BIOS, IOP, MMIO, or other external code).
//
// max_dispatches must be non-zero. The caller owns the metadata and entry
// arrays for the entire call; entries[i] corresponds to compiled->functions[i].
s_ee_dispatch_result ee_dispatch_run(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    uint32_t max_dispatches);

} // namespace ee
