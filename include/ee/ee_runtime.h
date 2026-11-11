#pragma once
#include "ee_state.h"
#include <cstdint>
#include <cstddef>

namespace ee {

struct s_ee_runtime
{
    // Opaque to the lifter; owned by the host application.
    void* m_user;
};

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
