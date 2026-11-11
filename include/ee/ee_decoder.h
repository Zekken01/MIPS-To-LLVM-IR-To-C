#pragma once
#include "ee_instruction.h"

namespace ee {

// Decode a single 32-bit R5900 instruction at guest pc into a normalized,
// LLVM-independent representation. The decoder never silently substitutes a
// result for an unsupported instruction: unknown/reserved encodings are
// marked UNSUPPORTED/INVALID with their guest address and opcode word.
s_ee_instruction decode(uint32_t pc, uint32_t word);

} // namespace ee
