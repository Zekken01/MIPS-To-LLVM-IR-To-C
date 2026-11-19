#pragma once

#include <cstdint>

namespace iop
{
class c_cpu;

// HLE for PS2SDK SYSCLIB ordinals 20..34. All pointers are IOP guest
// addresses; unsupported ROM/MMIO/TLB addresses fail without modification.
bool execute_sysclib_string(c_cpu& memory, uint32_t ordinal);
} // namespace iop
