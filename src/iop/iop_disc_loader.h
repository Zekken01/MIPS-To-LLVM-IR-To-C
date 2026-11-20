#pragma once

#include "iop_kernel.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iop
{
// A LOADFILE backend for an actual guest-supplied cdrom0: IRX pathname.
// This resolves one request; it neither scans ELF strings nor auto-starts
// arbitrary modules at IOP power-on. No SIF readiness or replies are faked.
enum class e_disc_load_result
{
    success,
    unsupported_device,
    invalid_guest_path,
    missing_file,
    ambiguous_file,
    invalid_file,
    io_error,
    link_failed,
    thread_failed
};

struct s_disc_load_result
{
    e_disc_load_result m_result = e_disc_load_result::invalid_guest_path;
    e_irx_load_result m_link_status = e_irx_load_result::invalid_elf;
    uint32_t m_module_id = 0u;
    uint32_t m_thread_id = 0u;
    s_irx_link_diagnostic m_link_diagnostic{};
    std::filesystem::path m_host_path;
};

class c_disc_loader
{
public:
    static constexpr uint64_t k_max_irx_bytes = 2u * 1024u * 1024u;

    explicit c_disc_loader(
        c_kernel& kernel, const std::filesystem::path& disc_root
    );

    // start_entrypoint is explicit because a LOADFILE RPC will want
    // module_start(), whereas a dependency-only bootstrap may first
    // link providers without starting their guest threads.
    s_disc_load_result load_requested(
        const std::string& guest_path,
        bool start_entrypoint,
        uint32_t priority = 1u,
        const std::vector<std::string>& arguments = {}
    );

private:
    c_kernel& m_kernel;
    std::filesystem::path m_disc_root;
};
} // namespace iop
