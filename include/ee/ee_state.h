#pragma once
#include "ee_instruction.h"
#include <cstdint>
#include <cstddef>

namespace ee {

struct alignas(16) s_ee_u128
{
    uint64_t m_lo;
    uint64_t m_hi;
};

// Architectural EE CPU state. GPRs are 128 bits wide in the lifter (i128);
// the runtime stores them as two 64-bit halves. Scalar instructions may write
// only the low word, low doubleword, or the full quadword depending on the op.
struct alignas(16) s_ee_cpu_state
{
    s_ee_u128 m_gpr[32];

    uint64_t m_hi;
    uint64_t m_lo;
    uint64_t m_hi1;
    uint64_t m_lo1;

    uint32_t m_sa;
    uint32_t m_pc;

    uint32_t m_exit_reason;
    uint32_t m_pad;

    // Coprocessor register files.
    uint32_t m_cop0[32];   // COP0 system control registers (Status, Cause, EPC, ...)
    uint64_t m_fpr[32];    // COP1 floating-point registers (32-bit or 64-bit values)
    s_ee_u128 m_vu0[32];   // COP2 VU0 vector registers (128-bit each)
    uint32_t m_fcsr;       // COP1 floating-point control/status (condition bit in FCC)
    uint32_t m_fpu_acc;    // COP1 accumulator (ADDA/MULA/MADD family)
};

enum class e_ee_exit_reason : uint32_t
{
    EE_EXIT_RETURN = 0,
    EE_EXIT_EXCEPTION = 1,
    EE_EXIT_EXTERNAL_CALL = 2,
    EE_EXIT_DISPATCH = 3,
    EE_EXIT_UNSUPPORTED = 4
};

// Forward declaration; the runtime owns the memory/peripheral subsystems.
struct s_ee_runtime;

// Stable C ABI entry point for a translated guest function.
// Returns an exit reason telling the host what to do next.
extern "C" uint32_t ee_entry(s_ee_cpu_state* cpu_state, s_ee_runtime* runtime);

} // namespace ee
