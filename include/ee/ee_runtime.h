#pragma once
#include "ee_state.h"
#include <cstdint>
#include <cstddef>

namespace ee {

enum class e_ee_vu0_sync_mode : uint32_t
{
    CATCH_UP_TO_EE_CYCLE = 0,
    FINISH_TO_E = 1,
    WAIT_TO_E_OR_M = 2
};

enum class e_ee_vu0_sync_result : uint32_t
{
    COMPLETE = 0,
    UNAVAILABLE = 1,
    INVALID_ARGUMENT = 2,
    FAILED = 3
};

enum class e_ee_vu0_transfer_direction : uint8_t
{
    FROM_VU0,
    TO_VU0
};

// Callback supplied by the host's VU0 scheduler. It must execute the requested
// synchronization policy, update cpu/vu0 state, and return an e_ee_vu0_sync_result.
// The user pointer is separate from m_user, which remains the EE memory context.
using t_ee_vu0_sync_callback = uint32_t (*)(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t mode,
    uint32_t pc
);

// Flags passed to the host VU0 micro-engine for one bounded execution slice.
enum e_ee_vu0_execute_flags : uint32_t
{
    EE_VU0_EXECUTE_STOP_AT_E = 1u << 0,
    EE_VU0_EXECUTE_STOP_AT_M = 1u << 1,
    EE_VU0_EXECUTE_EXACT_CYCLES = 1u << 2
};

// Execute at most max_cycles of VU0 work. The callback must update m_cycle,
// m_vpu_status and m_m_bit as execution proceeds. STOP_AT_E always applies
// to scheduler calls; STOP_AT_M is used only by WAIT_TO_E_OR_M. EXACT_CYCLES
// forbids the engine from running past a catch-up budget.
using t_ee_vu0_execute_callback = uint32_t (*)(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint64_t max_cycles,
    uint32_t flags,
    uint32_t pc
);

// PCSX2's interlocked COP2 paths also catch VU1 up after waiting for VU0.
using t_ee_vu0_vu1_catch_up_callback = uint32_t (*)(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t pc
);

// Schedule the EE event used to resume a still-running VU0.
using t_ee_vu0_schedule_event_callback = void (*)(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t delta_cycles,
    uint32_t pc
);

// Reusable timing-policy adapter. It supplies the PCSX2-style catch-up,
// finish-to-E and wait-to-E-or-M orchestration around a host VU micro-engine.
// The engine and VU1 callbacks remain host-provided; no fake engine is implied.
struct s_ee_vu0_scheduler
{
    void* m_user = nullptr;
    t_ee_vu0_execute_callback m_execute = nullptr;
    t_ee_vu0_vu1_catch_up_callback m_vu1_catch_up = nullptr;
    t_ee_vu0_schedule_event_callback m_schedule_event = nullptr;
    uint32_t m_max_run_calls = 65536;
};

// Can be assigned directly to s_ee_runtime::m_vu0_sync. user points to an
// s_ee_vu0_scheduler. Returns UNAVAILABLE when required host callbacks are absent.
uint32_t ee_vu0_scheduler_sync_callback(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t mode,
    uint32_t pc
);

// Host callback for CTC2 operations with external effects (FBRST and CMSAR1).
// It must perform the requested action or return UNAVAILABLE/FAILED without
// committing a partial action.
using t_ee_vu0_control_callback = uint32_t (*)(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t register_index,
    uint32_t value,
    uint32_t pc
);

struct s_ee_runtime
{
    // Opaque EE memory/runtime context; retained as the first ABI member.
    void* m_user;

    // VU0 architectural/scheduler state and optional host synchronization hook.
    // A missing state or callback fails closed; it is never treated as a no-op.
    s_ee_vu0_state* m_vu0_state;
    void* m_vu0_user;
    t_ee_vu0_sync_callback m_vu0_sync;

    // Optional host-side VU0/VU1 control actions for CTC2 special registers.
    void* m_vu0_control_user;
    t_ee_vu0_control_callback m_vu0_control;
};

// Outside a branch delay slot, all four transfer forms only catch VU0 up to
// the EE cycle. In a delay slot, reads finish to E while writes wait to E or M.
e_ee_vu0_sync_mode ee_vu0_transfer_sync_mode(
    e_ee_vu0_transfer_direction direction,
    bool in_delay_slot
);

#ifdef __cplusplus
extern "C" {
#endif

// Guest memory access helpers. These preserve guest address translation,
// alignment, address errors and MMIO/device side effects. Implementations live
// in the runtime (src/runtime/ee_memory.cpp). The lifter calls these rather
// than dereferencing host pointers directly.
uint8_t  ee_mem_read8 (s_ee_runtime* rt, uint32_t addr);
uint16_t ee_mem_read16(s_ee_runtime* rt, uint32_t addr);
uint32_t ee_mem_read32(s_ee_runtime* rt, uint32_t addr);
uint64_t ee_mem_read64(s_ee_runtime* rt, uint32_t addr);
void     ee_mem_read128(s_ee_runtime* rt, uint32_t addr, s_ee_u128* out);

void     ee_mem_write8 (s_ee_runtime* rt, uint32_t addr, uint8_t  v);
void     ee_mem_write16(s_ee_runtime* rt, uint32_t addr, uint16_t v);
void     ee_mem_write32(s_ee_runtime* rt, uint32_t addr, uint32_t v);
void     ee_mem_write64(s_ee_runtime* rt, uint32_t addr, uint64_t v);
void     ee_mem_write128(s_ee_runtime* rt, uint32_t addr, const s_ee_u128* v);

// VU0 synchronization and VI register access (ee_vu0.cpp).
uint32_t ee_vu0_sync(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                     uint32_t mode, uint32_t pc);
uint32_t ee_vu0_read_vi(const s_ee_vu0_state* vu0, uint32_t index);
void     ee_vu0_write_vi(s_ee_vu0_state* vu0, uint32_t index, uint32_t value);

// Synchronized VU0 transfers. The indices are decoded 5-bit fields: gpr_index
// is rt and vu_index is fs/rd. Destinations obey hardwired GPR0/VF0 semantics.
uint32_t ee_vu0_qmfc2(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                      uint32_t gpr_index, uint32_t vf_index,
                      uint32_t mode, uint32_t pc);
uint32_t ee_vu0_qmtc2(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                      uint32_t gpr_index, uint32_t vf_index,
                      uint32_t mode, uint32_t pc);
uint32_t ee_vu0_cfc2(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                      uint32_t gpr_index, uint32_t vi_index,
                      uint32_t mode, uint32_t pc);
uint32_t ee_vu0_ctc2(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                      uint32_t gpr_index, uint32_t vi_index,
                      uint32_t mode, uint32_t pc);

// BC2 reads VPU_STAT bit 8 directly, as the interpreter branch handlers do.
// Returns 0/1 for false/true, or 2 when VU0 state is unavailable.
uint32_t ee_vu0_get_branch_condition(const s_ee_runtime* rt);

// Exception entry / return and interrupt helpers (ee_exceptions.cpp).
uint32_t ee_raise_exception(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                            uint32_t excode, uint32_t pc,
                            uint32_t in_bd_slot, uint32_t badvaddr);
uint32_t ee_exception_return(s_ee_cpu_state* cpu);
void     ee_interrupt_request(s_ee_cpu_state* cpu, uint32_t line);
void     ee_interrupt_clear(s_ee_cpu_state* cpu, uint32_t line);
uint32_t ee_check_interrupts(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                             uint32_t pc, uint32_t in_bd_slot);
void     ee_timer_tick(s_ee_cpu_state* cpu, uint32_t cycles);

#ifdef __cplusplus
}
#endif

} // namespace ee
