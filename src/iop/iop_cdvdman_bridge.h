#pragma once

#include "iop_folder_sector_source.h"
#include "iop_kernel.h"

#include <cstdint>
#include <string>

namespace iop
{
// Only CDVDMAN ordinal 10 is implemented. A real guest IRX import
// must call the trampoline; installing this bridge does not read media,
// complete a CDVD command or publish any SIF readiness signal.
class c_cdvdman_bridge
{
public:
    static constexpr uint32_t k_search_file_stub = 0x00018200u;
    static constexpr uint32_t k_search_file_syscall = 0x494f020au;

    c_cdvdman_bridge(c_kernel& kernel, const c_folder_sector_source& source);
    bool install();
    bool handle_syscall(c_kernel& kernel, uint32_t service);

private:
    static bool ram_pointer(uint32_t guest, uint32_t size,
                            uint32_t* physical);
    bool read_path(uint32_t pointer, std::string* path) const;

    c_kernel& m_kernel;
    const c_folder_sector_source& m_source;
};
} // namespace iop
