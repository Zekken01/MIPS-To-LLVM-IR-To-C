#pragma once

#include "iop_cpu.h"
#include "iop_hw.h"
#include "irx_loader.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace iop
{
// Deliberately separate IOP kernel threads from EE guest threads.
// A syscall requires a real kernel handler; unknown requests stop execution.
enum class e_thread_status
{
    dormant,
    ready,
    running,
    blocked,
    faulted
};

enum class e_kernel_stop
{
    budget_exhausted,
    idle,
    unhandled_syscall,
    unsupported_instruction,
    memory_fault,
    arithmetic_overflow,
    breakpoint
};

struct s_kernel_run_result
{
    e_kernel_stop m_stop = e_kernel_stop::budget_exhausted;
    uint64_t m_executed = 0u;
    uint32_t m_thread_id = 0u;
    uint32_t m_guest_pc = 0u;
    uint32_t m_opcode = 0u;
};

struct s_kernel_thread
{
    uint32_t m_id = 0u;
    uint32_t m_priority = 0u;
    uint32_t m_stack_base = 0u;
    uint32_t m_stack_size = 0u;
    uint64_t m_wake_cycle = 0u;
    e_thread_status m_status = e_thread_status::dormant;
    s_cpu_state m_cpu{};
};

struct s_kernel_module
{
    uint32_t m_id = 0u;
    s_irx_module m_irx{};
};

struct s_module_load_result
{
    e_irx_load_result m_status = e_irx_load_result::invalid_module;
    uint32_t m_id = 0u;
    s_irx_link_diagnostic m_link_diagnostic{};
};

// Called after the R3000 encounters a syscall. The callback may update
// the CPU registers and call sleep_current()/stop_current(). Returning
// false leaves the syscall unhandled and stops at the original guest PC.
// No default kernel HLE, SIF service, or synthetic ready bit exists.
class c_kernel;
using t_kernel_syscall = std::function<bool(
    c_kernel& kernel, uint32_t syscall_number
)>;

class c_kernel
{
public:
    static constexpr uint32_t k_user_base = 0x00020000u;

    c_kernel();
    void reset();

    // The authentic R3000 bootstrap runs directly from the caller's
    // ROM0 image at BFC00000, not as a fabricated kernel thread.
    // This must occur before any IRX imports or modules are installed.
    bool start_rom0_boot(const std::vector<uint8_t>& image);
    bool rom0_boot_active() const;
    c_hw& hardware();
    const c_hw& hardware() const;

    c_cpu& cpu();
    const c_cpu& cpu() const;
    uint64_t cycles() const;
    uint32_t current_thread_id() const;

    // Actual IOP library exports supply concrete executable targets.
    bool lookup_library(
        const std::string& name, uint16_t ordinal,
        uint32_t* entry
    ) const;
    bool register_library(
        const std::string& name, uint16_t ordinal, uint32_t entry
    );
    s_module_load_result load_module(
        const std::vector<uint8_t>& bytes
    );
    const s_kernel_module* module(uint32_t id) const;

    // HLE IOPHEAP shares the same first-fit IOP RAM address space
    // as linked IRX modules and guest thread stacks. Only addresses
    // allocated by this API can be freed through IOPHEAP.
    uint32_t allocate_heap(uint32_t size);
    bool free_heap(uint32_t address);
    // Enforce ownership for guest-requested file copies into IOP RAM.
    bool owns_heap_span(uint32_t address, uint32_t size) const;
    uint32_t heap_max_free() const;
    uint32_t heap_total_free() const;

    // Create an initially dormant R3000 thread. Priority 0 is highest.
    // A kernel will eventually publish proper thread APIs to IRX imports.
    uint32_t create_thread(
        uint32_t entry, uint32_t gp, uint32_t priority,
        uint32_t stack_size = 0x1000u,
        uint32_t a0 = 0u, uint32_t a1 = 0u
    );
    uint32_t create_module_thread(
        uint32_t module_id, uint32_t priority,
        uint32_t stack_size = 0x4000u
    );
    bool start_thread(uint32_t id);
    bool start_thread_with_argument(uint32_t id, uint32_t argument);
    bool wake_thread(uint32_t id);
    bool stop_thread(uint32_t id);
    bool sleep_current(uint64_t delay_cycles);
    bool block_current();
    const s_kernel_thread* thread(uint32_t id) const;
    // Only scheduled guest work permits an HLE transport-triggered
    // IOP execution turn; absent modules cannot advance IOP time.
    bool has_guest_work() const;

    void bind_syscall(t_kernel_syscall handler);
    // Explicit IOP cycle budget. The EE must eventually provide this from
    // guest execution timing, never by counting native dispatches.
    s_kernel_run_result run(
        uint64_t cycle_budget, uint32_t quantum = 128u
    );

private:
    uint32_t allocate(uint32_t size);
    bool estimate_module_size(
        const std::vector<uint8_t>& bytes, uint32_t* size
    ) const;
    uint32_t next_ready() const;
    void wake_due();

    c_cpu m_cpu{};
    c_hw m_hardware{};
    std::map<uint32_t, uint32_t> m_allocations;
    std::map<uint32_t, uint32_t> m_heap_allocations;
    std::map<std::string, std::map<uint16_t, uint32_t>> m_libraries;
    std::map<uint32_t, s_kernel_module> m_modules;
    std::map<uint32_t, s_kernel_thread> m_threads;
    t_kernel_syscall m_syscall;
    uint64_t m_cycles = 0u;
    bool m_rom0_boot_active = false;
    uint32_t m_next_module_id = 1u;
    uint32_t m_next_thread_id = 1u;
    uint32_t m_last_scheduled_id = 0u;
    uint32_t m_current_thread_id = 0u;
};
} // namespace iop
