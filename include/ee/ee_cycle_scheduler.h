#pragma once

#include <cstdint>

namespace ee
{
class c_ee_hw_bus;
class c_ee_vsync_flags;
class ee_memory;

// A native timing source must be validated independently against R5900
// reference traces before this scheduler can drive hardware. The native
// runner intentionally does not install a source until that work is done.
enum class e_ee_cycle_source : uint8_t
{
    UNAVAILABLE = 0,
    VALIDATED_R5900 = 1
};

enum class e_ee_scheduled_event_result : uint8_t
{
    NO_VALIDATED_CLOCK = 0,
    NO_CONFIGURED_SCANOUT,
    NO_DUE_FIELD,
    FIELDS_DELIVERED,
    INVALID_TIMESTAMP_OR_DELIVERY
};

// Owns one monotonic EE time domain, distinct from host dispatches, guest
// instruction attempts and reference-interpreter slots. This class never
// computes instruction latencies or enables a clock by itself.
//
// Integration contract: only an independently validated R5900 timing
// producer may install the clock and publish retirement/issue timestamps.
// Publishers must do so in guest execution order. Service at architectural
// safepoints after committing the corresponding CPU/memory effects.
class c_ee_cycle_scheduler
{
public:
    void reset();
    bool install_validated_source();
    bool publish_cycle(uint64_t absolute_cycle);
    void record_scheduler_yield();

    e_ee_scheduled_event_result service_scanout(
        c_ee_hw_bus& hardware,
        c_ee_vsync_flags& vsync_flags,
        ee_memory& memory
    );

    e_ee_cycle_source source() const;
    uint64_t current_cycle() const;
    uint64_t scheduler_yields() const;

private:
    uint64_t m_current_cycle = 0u;
    uint64_t m_scheduler_yields = 0u;
    e_ee_cycle_source m_source = e_ee_cycle_source::UNAVAILABLE;
};

} // namespace ee
