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

struct s_ee_runtime
{
    // Opaque EE memory/runtime context; retained as the first ABI member.
    void* m_user;

    // VU0 architectural/scheduler state and optional host synchronization hook.
    // A missing state or callback fails closed; it is never treated as a no-op.
    s_ee_vu0_state* m_vu0_state;
    void* m_vu0_user;
    t_ee_vu0_sync_callback m_vu0_sync;
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
