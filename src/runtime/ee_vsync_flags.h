#pragma once

#include <cstdint>

namespace ee
{

class ee_memory;

// Kernel SetVSyncFlag (0x73) registers guest-RAM destinations. Registration
// never acknowledges or synthesizes a VSync: a genuine GS vertical-blank
// producer must explicitly deliver the hardware CSR value.
class c_ee_vsync_flags
{
public:
    bool register_guest_pointers(
        const ee_memory* memory,
        uint32_t flag_address,
        uint32_t csr_address
    );

    // Only to be called after a genuine GS VSync event. No native runner
    // currently generates such an event merely to advance guest execution.
    bool deliver_actual_vsync(ee_memory* memory, uint64_t gs_csr);

    uint32_t flag_address() const;
    uint32_t csr_address() const;

private:
    uint32_t m_flag_address = 0;
    uint32_t m_csr_address = 0;
};

} // namespace ee
