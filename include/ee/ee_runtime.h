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

constexpr uint32_t EE_VU0_DATA_MEMORY_SIZE = 0x1000u;
constexpr uint32_t EE_VU0_MICRO_MEMORY_SIZE = 0x1000u;
constexpr uint32_t EE_VU0_MICRO_MEMORY_MASK = EE_VU0_MICRO_MEMORY_SIZE - 1u;

enum e_ee_vu0_micro_flags : uint32_t
{
    EE_VU0_MICRO_FLAG_I = 1u << 0,
    EE_VU0_MICRO_FLAG_E = 1u << 1,
    EE_VU0_MICRO_FLAG_M = 1u << 2,
    EE_VU0_MICRO_FLAG_D = 1u << 3,
    EE_VU0_MICRO_FLAG_T = 1u << 4
};

// Decode identities from the PCSX2 VU0 primary/secondary opcode tables.
// These values classify encodings only; none imply that execution is implemented.
enum class e_ee_vu0_micro_opcode : uint16_t
{
    UNKNOWN = 0,

    LQ, SQ, ILW, ISW, IADDIU, ISUBIU,
    FCEQ, FCSET, FCAND, FCOR, FSEQ, FSSET, FSAND, FSOR,
    FMEQ, FMAND, FMOR, FCGET,
    B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ, IBGEZ,
    IADD, ISUB, IADDI, IAND, IOR,

    MOVE, LQI, DIV, MTIR, RNEXT, MFP, XTOP, XGKICK,
    ESADD, EATAN_XY, ESQRT, ESIN,
    MR32, SQI, SQRT, MFIR, RGET, XITOP,
    ERSADD, EATAN_XZ, ERSQRT, EATAN,
    LQD, RSQRT, ILWR, RINIT, ELENG, ESUM, ERCPR, EEXP,
    SQD, WAITQ, ISWR, RXOR, ERLENG, WAITP,

    ADD_X, ADD_Y, ADD_Z, ADD_W,
    SUB_X, SUB_Y, SUB_Z, SUB_W,
    MADD_X, MADD_Y, MADD_Z, MADD_W,
    MSUB_X, MSUB_Y, MSUB_Z, MSUB_W,
    MAX_X, MAX_Y, MAX_Z, MAX_W,
    MINI_X, MINI_Y, MINI_Z, MINI_W,
    MUL_X, MUL_Y, MUL_Z, MUL_W,
    MUL_Q, MAX_I, MUL_I, MINI_I,
    ADD_Q, MADD_Q, ADD_I, MADD_I,
    SUB_Q, MSUB_Q, SUB_I, MSUB_I,
    ADD, MADD, MUL, MAX, SUB, MSUB, OPMSUB, MINI,

    ADDA_X, SUBA_X, MADDA_X, MSUBA_X, ITOF0, FTOI0, MULA_X, MULA_Q,
    ADDA_Q, SUBA_Q, ADDA, SUBA,
    ADDA_Y, SUBA_Y, MADDA_Y, MSUBA_Y, ITOF4, FTOI4, MULA_Y, ABS,
    MADDA_Q, MSUBA_Q, MADDA, MSUBA,
    ADDA_Z, SUBA_Z, MADDA_Z, MSUBA_Z, ITOF12, FTOI12, MULA_Z,
    MULA_I, ADDA_I, SUBA_I, MULA, OPMULA,
    ADDA_W, SUBA_W, MADDA_W, MSUBA_W, ITOF15, FTOI15, MULA_W,
    CLIP, MADDA_I, MSUBA_I, NOP
};

// Bit fields shared by lower and upper VU micro-op encodings. xyzw uses
// bit 3 for X, bit 2 for Y, bit 1 for Z and bit 0 for W.
struct s_ee_vu0_micro_op_fields
{
    uint8_t m_opcode;
    uint8_t m_ft;
    uint8_t m_fs;
    uint8_t m_fd;
    uint8_t m_it;
    uint8_t m_is;
    uint8_t m_id;
    uint8_t m_xyzw;
    e_ee_vu0_micro_opcode m_operation;
};

// VU micro-instructions are 64-bit pairs: lower word at +0, upper word at +4.
// m_pc is a byte address in the 4 KB micro-program memory. Flags are decoded
// from the upper word and normalized to e_ee_vu0_micro_flags. Each half also
// carries the primary/secondary opcode identity; classification does not
// imply that the micro-op has an execution implementation.
struct s_ee_vu0_micro_instruction
{
    uint32_t m_pc;
    uint32_t m_lower;
    uint32_t m_upper;
    uint32_t m_flags;
    s_ee_vu0_micro_op_fields m_lower_fields;
    s_ee_vu0_micro_op_fields m_upper_fields;
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

// Optional host-owned EE hardware-register dispatcher. The address is the
// translated physical bus address, and width is one of 1, 2, 4, 8, 16.
// The caller zero-initializes *value. Return true only when the device
// actually handled the access; false means unmapped/unimplemented.
// A 128-bit access is a SINGLE callback, never 2 separate 64-bit accesses,
// so FIFO and other side-effectful registers can behave correctly.
using t_ee_mmio_read_callback = bool (*)(
    void* user, uint32_t physical_address, uint32_t width, s_ee_u128* value
);
using t_ee_mmio_write_callback = bool (*)(
    void* user, uint32_t physical_address, uint32_t width, const s_ee_u128* value
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

    // Installed by the host, never implicitly emulated by the recompiler.
    // No device callbacks means reads return zero and writes are ignored.
    void* m_mmio_user = nullptr;
    t_ee_mmio_read_callback m_mmio_read = nullptr;
    t_ee_mmio_write_callback m_mmio_write = nullptr;

    // Host diagnostic guard against translated guest loops that never
    // return to the dispatcher. Zero limit disables the guard; all new
    // fields are appended to preserve the existing runtime ABI prefix.
    // The host resets remaining before entering one native guest function.
    uint32_t m_loop_safepoint_limit = 0u;
    uint32_t m_loop_safepoint_remaining = 0u;

    // Optional, host-only trace of translated guest STORE instructions.
    // A zero-initialized runtime never traces. Keep these fields appended
    // to preserve all existing ABI offsets.
    uint32_t m_guest_store_trace_word = 0u;
    uint32_t m_guest_store_trace_remaining = 0u;
    bool m_guest_store_trace_enabled = false;

    // Optional PC-bounded diagnostic for guest V0 assignments.
    // Remaining == 0 disables the trace; appended for ABI compatibility.
    uint32_t m_guest_v0_trace_first_pc = 0u;
    uint32_t m_guest_v0_trace_last_pc = 0u;
    uint32_t m_guest_v0_trace_remaining = 0u;

    // Diagnostic count of reached, decodable guest EE instructions. This
    // includes executed delay slots but not annulled branch-likely slots,
    // and does not promote one instruction to one EE cycle (R5900 is
    // superscalar and instructions can stall). Appended for ABI stability.
    uint64_t m_guest_instruction_attempts = 0u;

    // Opt-in host scheduling mode for translated loop-header safepoints.
    // Default false keeps the existing terminal LOOP_BUDGET diagnostic.
    // A HOST_YIELD never creates a clock tick, VBlank or guest exception.
    uint32_t m_loop_safepoint_yield_enabled = 0u;
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

// Guest memory access helpers. The prototype maps bounded EE RAM,
// scratchpad, and VU0 memories, and can dispatch 0x10000000..0x12ffffff
// hardware bus addresses through host-provided MMIO callbacks. It does NOT
// implement the TLB, alignment exceptions, VU1, or actual hardware devices.
// The lifter calls these rather than dereferencing host pointers.
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

// Diagnostic only: emitted immediately BEFORE a guest memory store.
// The SSA operands are sampled at the instruction, including in delay slots.
// 'affected_address' is the aligned containing word/doubleword for partial
// stores; 'effective_address' preserves the actual guest address.
void ee_trace_guest_store(
    s_ee_runtime* rt, uint32_t pc, uint32_t opcode,
    uint32_t effective_address, uint32_t affected_address,
    uint32_t width, uint64_t source_value, uint32_t source_lane,
    uint64_t base_value, uint64_t sp, uint64_t fp, uint64_t ra
);

// Reports the exact instruction which changes the low 64 bits of GPR V0.
// Called with the emitter's SSA old/new values, not the stale CPU backing
// store, and does not modify architectural CPU or memory state.
void ee_trace_guest_v0_write(
    s_ee_runtime* rt, uint32_t pc, uint32_t opcode,
    uint64_t old_value, uint64_t new_value
);

// R5900 partial-word / partial-doubleword memory operations.
// The load helpers merge into the existing GPR low 64 bits and return the
// resulting low 64 bits; the caller preserves the upper 64 bits of the GPR.
// Address is the original effective address, not the aligned word address.
// These merge semantics are independent of host memory layout and use the
// ordinary ee_mem_* callbacks so device/bounds handling remains centralized.
uint64_t ee_mem_lwl(s_ee_runtime* rt, uint32_t addr, uint64_t old_value);
uint64_t ee_mem_lwr(s_ee_runtime* rt, uint32_t addr, uint64_t old_value);
uint64_t ee_mem_ldl(s_ee_runtime* rt, uint32_t addr, uint64_t old_value);
uint64_t ee_mem_ldr(s_ee_runtime* rt, uint32_t addr, uint64_t old_value);
void ee_mem_swl(s_ee_runtime* rt, uint32_t addr, uint64_t source);
void ee_mem_swr(s_ee_runtime* rt, uint32_t addr, uint64_t source);
void ee_mem_sdl(s_ee_runtime* rt, uint32_t addr, uint64_t source);
void ee_mem_sdr(s_ee_runtime* rt, uint32_t addr, uint64_t source);

// VU0 synchronization and VI register access (ee_vu0.cpp).
uint32_t ee_vu0_sync(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                     uint32_t mode, uint32_t pc);
uint32_t ee_vu0_read_vi(const s_ee_vu0_state* vu0, uint32_t index);
void     ee_vu0_write_vi(s_ee_vu0_state* vu0, uint32_t index, uint32_t value);

// Fetch one aligned 64-bit VU0 micro-instruction pair from m_micro_memory.
// m_micro_pc is a byte offset and wraps at the 4 KB program-memory boundary.
// The PC advances only after a successful fetch. Returns COMPLETE or
// INVALID_ARGUMENT; unsupported opcode handling belongs to the micro-engine.
uint32_t ee_vu0_fetch_micro_instruction(
    s_ee_vu0_state* vu0,
    s_ee_vu0_micro_instruction* out
);

// Conservative VU0 interpreter slice: lower integer ALU/control-flow and
// ordinary data-memory ILW/ISW, LQ/SQ, and indexed LQI/SQI/LQD/SQD
// operations paired with upper NOP only.
// VU1-mapped register memory, upper vector arithmetic and I/D/T flags remain
// unsupported; rejected pairs leave the micro-PC at the pair start.
struct s_ee_vu0_ialu_engine
{
    uint32_t m_max_instructions_per_call = 65536;
};

uint32_t ee_vu0_execute_ialu_callback(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint64_t max_cycles,
    uint32_t flags,
    uint32_t pc
);

// IALU reservation queue. A schedule call records the destination mask and
// latency at the current VU cycle. Read-stall advances m_cycle to the latest
// completion among pending entries that overlap read_mask. Retire removes
// completed entries from the FIFO; flush-all advances to and retires all.
uint32_t ee_vu0_ialu_schedule_write(
    s_ee_vu0_state* vu0,
    uint32_t register_mask,
    uint32_t latency
);
uint32_t ee_vu0_ialu_retire_matured(
    s_ee_vu0_state* vu0,
    uint32_t* retired_count
);
uint32_t ee_vu0_ialu_wait_for_reads(
    s_ee_vu0_state* vu0,
    uint32_t read_mask
);
uint32_t ee_vu0_ialu_flush_all(s_ee_vu0_state* vu0);

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

// Executes only VMULAx/VMADDAy/VMADDAz/VMADDw (the SDBZ US matrix
// transform), synchronized against genuine VU0 execution. Invalid encodings
// and unavailable synchronization must not commit VF/ACC/MAC/STATUS state.
uint32_t ee_vu0_macro_fmac(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                            uint32_t instruction_word, uint32_t pc);

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
