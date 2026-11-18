#pragma once

#include "iop_kernel.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace iop
{
// Guest-callable IOP kernel services used by the SDBZ module family.
// Only explicitly implemented ordinals have MIPS R3000 syscall stubs;
// unknown imports and unsupported hardware operations remain fail-closed.
class c_kernel_hle_bridge
{
public:
    static constexpr uint32_t k_stub_base = 0x00014000u;
    static constexpr uint32_t k_service_base = 0x494f1000u;

    explicit c_kernel_hle_bridge(c_kernel& kernel);

    bool install();
    void reset_for_reboot();
    bool handle_syscall(c_kernel& kernel, uint32_t service);

private:
    struct s_interrupt
    {
        uint32_t m_mode = 0u;
        uint32_t m_handler = 0u;
        uint32_t m_argument = 0u;
    };

    struct s_semaphore
    {
        uint32_t m_attr = 0u;
        uint32_t m_option = 0u;
        uint32_t m_count = 0u;
        uint32_t m_maximum = 0u;
        std::deque<uint32_t> m_waiters;
    };

    struct s_event_waiter
    {
        uint32_t m_thread = 0u;
        uint32_t m_mask = 0u;
        uint32_t m_mode = 0u;
        uint32_t m_output = 0u;
    };

    struct s_event
    {
        uint32_t m_attr = 0u;
        uint32_t m_option = 0u;
        uint32_t m_bits = 0u;
        std::vector<s_event_waiter> m_waiters;
    };

    bool read_string(uint32_t pointer, uint32_t maximum,
                     std::string* output) const;
    bool read_argument(uint32_t index, uint32_t* value) const;
    bool write_console(const std::string& content);
    bool register_loaded_guest_library(uint32_t table);
    bool match_event(const s_event& event, uint32_t bits,
                     uint32_t mode) const;
    bool take_event(s_event& event, uint32_t bits, uint32_t mode,
                    uint32_t output);
    bool handle_loadcore(uint32_t ordinal);
    bool handle_intrman(uint32_t ordinal);
    bool handle_stdio(uint32_t ordinal);
    bool handle_dmacman(uint32_t ordinal);
    bool handle_thbase(uint32_t ordinal);
    bool handle_thevent(uint32_t ordinal);
    bool handle_thsemap(uint32_t ordinal);
    bool handle_sysclib(uint32_t ordinal);

    c_kernel& m_kernel;
    std::map<uint32_t, s_interrupt> m_interrupts;
    std::map<uint32_t, s_semaphore> m_semaphores;
    std::map<uint32_t, s_event> m_events;
    uint32_t m_dma_direction[16]{};
    uint32_t m_next_object = 1u;
    bool m_cpu_interrupts_suspended = false;
    bool m_installed = false;
};
} // namespace iop
