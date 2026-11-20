#pragma once

#include <cstdint>

namespace iop
{
class c_cpu;

// HLE for PS2SDK SYSCLIB ordinals 20..34. All pointers are IOP guest
// addresses; unsupported ROM/MMIO/TLB addresses fail without modification.
bool execute_sysclib_string(c_cpu& memory, uint32_t ordinal);

// SYSCLIB sprintf (#19) and vsprintf (#42) with real O32/guest
// va_list argument access. Formats only validated guest data, then
// atomically writes the complete NUL-terminated result to IOP RAM.
bool execute_sysclib_format(c_cpu& memory, uint32_t ordinal);
} // namespace iop
