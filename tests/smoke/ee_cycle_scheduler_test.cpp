#include "ee_cycle_scheduler.h"
#include "ee_runtime.h"
#include "ee_hw_bus.h"
#include "ee_vsync_flags.h"
#include "ee_memory.h"

#include <cstdint>
#include <cstdio>

namespace
{
int g_failures = 0;

void check(bool condition, const char* description)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}
}

int main()
{
    ee::s_ee_runtime runtime{};
    ee::c_ee_hw_bus hardware{};
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_vsync_flags vsync_flags{};
    ee::ee_memory memory(0x1000u);
    hardware.bind_cpu(&cpu);

    auto& timeline = runtime.m_ee_cycle_scheduler;
    using result = ee::e_ee_scheduled_event_result;

    check(hardware.set_gs_crt(1u, 2u, 0u),
          "native NTSC scanout config is accepted");
    check(vsync_flags.register_guest_pointers(&memory, 0x100u, 0x108u),
          "store one-shot guest pointers");
    memory.write32(0x100u, 0xfeedfaceu);
    memory.write64(0x108u, 0x1111111111111111ull);
    check(!timeline.publish_cycle(4920115ull) &&
          timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::NO_VALIDATED_CLOCK &&
          hardware.scanout().fields_started() == 0u &&
          hardware.intc().status() == 0u &&
          memory.read32(0x100u) == 0xfeedfaceu,
          "unvalidated or absent cycles never generate VBlank");

    // A synthetic deterministic test source exercises the scheduling
    // contract. It is NOT evidence of calibrated R5900 instruction timing.
    check(timeline.install_validated_source() &&
          !timeline.install_validated_source(),
          "validated-source installation is explicit and one-time");
    check(timeline.publish_cycle(4920114ull) &&
          timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::NO_DUE_FIELD &&
          hardware.scanout().fields_started() == 0u &&
          memory.read32(0x100u) == 0xfeedfaceu,
          "no premature field at first NTSC deadline minus one");
    check(!timeline.publish_cycle(4920113ull) &&
          timeline.current_cycle() == 4920114ull,
          "backwards timestamps are rejected without state mutation");
    check(timeline.publish_cycle(4920115ull) &&
          timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::FIELDS_DELIVERED &&
          hardware.scanout().fields_started() == 1u &&
          hardware.intc().status() == 4u &&
          memory.read32(0x100u) == 1u &&
          memory.read64(0x108u) == hardware.scanout().csr() &&
          vsync_flags.flag_address() == 0u,
          "genuine field boundary delivers CSR, INTC and one-shot flag");
    check(timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::NO_DUE_FIELD &&
          hardware.scanout().fields_started() == 1u,
          "re-entering the scheduler does not duplicate a field");
    memory.write32(0x100u, 0x12345678u);
    check(timeline.publish_cycle(hardware.scanout().next_field_cycle()) &&
          timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::FIELDS_DELIVERED &&
          hardware.scanout().fields_started() == 2u &&
          memory.read32(0x100u) == 0x12345678u,
          "later fields do not reuse consumed guest pointers");
    check(vsync_flags.register_guest_pointers(&memory, 0x100u, 0x108u) &&
          timeline.publish_cycle(hardware.scanout().next_field_cycle()) &&
          timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::FIELDS_DELIVERED &&
          memory.read32(0x100u) == 1u,
          "guest re-registration delivers on a subsequent real field");

    timeline.record_scheduler_yield();
    timeline.record_scheduler_yield();
    check(timeline.scheduler_yields() == 2u &&
          timeline.current_cycle() == hardware.scanout().last_ee_cycle(),
          "host yields never advance hardware time");
    timeline.reset();
    check(timeline.current_cycle() == 0u &&
          timeline.scheduler_yields() == 0u &&
          timeline.source() == ee::e_ee_cycle_source::UNAVAILABLE &&
          timeline.service_scanout(hardware, vsync_flags, memory) ==
              result::NO_VALIDATED_CLOCK,
          "reset disables events until a new validated source is installed");

    // Reference-compatible budgets use the real event delivery path,
    // but never claim independent hardware-cycle validation.
    ee::c_ee_hw_bus compatibility_hardware{};
    ee::s_ee_cpu_state compatibility_cpu{};
    ee::c_ee_vsync_flags compatibility_flags{};
    ee::ee_memory compatibility_memory(0x1000u);
    compatibility_hardware.bind_cpu(&compatibility_cpu);
    check(compatibility_hardware.set_gs_crt(1u, 2u, 0u) &&
          compatibility_flags.register_guest_pointers(
              &compatibility_memory, 0x100u, 0x108u) &&
          timeline.install_reference_compatibility_source() &&
          timeline.source() == ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY &&
          !timeline.install_validated_source(),
          "reference compatibility source is distinct from validated R5900");
    check(timeline.publish_cycle(4920114ull) &&
          timeline.service_scanout(
              compatibility_hardware, compatibility_flags, compatibility_memory) ==
              result::NO_DUE_FIELD &&
          compatibility_hardware.scanout().fields_started() == 0u,
          "compatibility source cannot deliver before field deadline");
    check(timeline.publish_cycle(4920115ull) &&
          timeline.service_scanout(
              compatibility_hardware, compatibility_flags, compatibility_memory) ==
              result::FIELDS_DELIVERED &&
          compatibility_hardware.scanout().fields_started() == 1u &&
          compatibility_hardware.intc().status() == 4u &&
          compatibility_memory.read32(0x100u) == 1u &&
          !timeline.publish_cycle(4920114ull),
          "compatibility deadline delivers CSR/INTC/VSync and rejects regressions");

    std::printf("ee_cycle_scheduler_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
