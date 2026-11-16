#pragma once

#include "iop_cpu.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iop
{
enum class e_irx_load_result
{
    success,
    invalid_elf,
    invalid_bounds,
    invalid_module,
    unsupported_relocation,
    unresolved_import,
    invalid_import_target,
    insufficient_ram
};

struct s_irx_module
{
    std::string m_name;
    uint32_t m_base = 0u;
    uint32_t m_size = 0u;
    uint32_t m_entry = 0u;
    uint32_t m_gp = 0u;
};

// The registered IOP kernel/IRX libraries will supply import targets.
// An absent resolver never invents HLE implementations.
using t_irx_import_resolver = std::function<bool(
    const std::string& library, uint16_t ordinal, uint32_t* target
)>;

// Load one Sony IOP ELF32 MIPS IRX at an explicitly allocated, aligned IOP
// RAM base. All parsing, relocation and import linking are performed on a
// private staging buffer. On failure, guest memory and output stay unchanged.
//
// No module thread is started, no syscalls are serviced, and no EE SIF
// status is altered. Allocation/scheduling are owned by the future kernel.
e_irx_load_result load_irx(
    c_cpu& cpu,
    const std::vector<uint8_t>& bytes,
    uint32_t base,
    s_irx_module* module,
    const t_irx_import_resolver& resolver = {}
);
} // namespace iop
