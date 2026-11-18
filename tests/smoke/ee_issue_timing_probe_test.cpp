#include "ee_issue_timing_probe.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{
int g_failures = 0;

void check(bool value, const char* label)
{
    if (!value)
    {
        std::printf("FAIL: %s\n", label);
        ++g_failures;
    }
}
}

int main()
{
    using ee::c_ee_issue_timing_probe;
    using ee::e_ee_probe_pipe;
    using ee::s_ee_probe_instruction;

    c_ee_issue_timing_probe clock{};
    uint64_t at = 99u;
    const s_ee_probe_instruction a{1u << 1u, 1u << 2u};
    const s_ee_probe_instruction b{1u << 3u, 1u << 4u};
    const s_ee_probe_instruction c{1u << 5u, 1u << 6u};

    check(clock.issue(a, &at) && at == 0u &&
          clock.issue(b, &at) && at == 0u &&
          clock.issue(c, &at) && at == 1u &&
          clock.estimated_cycles() == 2u &&
          clock.instructions_issued() == 3u,
          "independent integer instructions can pair twice per cycle");

    clock.reset();
    const s_ee_probe_instruction dependency{1u << 2u, 1u << 7u};
    check(clock.issue(a) && clock.issue(dependency, &at) &&
          at == 1u && clock.estimated_cycles() == 2u,
          "RAW dependency prevents same-cycle dual issue");

    clock.reset();
    const s_ee_probe_instruction load{
        1u << 1u, 1u << 2u, e_ee_probe_pipe::LOAD, 3u
    };
    check(clock.issue(load, &at) && at == 0u &&
          clock.issue(dependency, &at) && at == 3u &&
          clock.estimated_cycles() == 4u,
          "caller-declared load latency stalls a dependent instruction");

    clock.reset();
    const s_ee_probe_instruction branch{
        1u << 1u, 0u, e_ee_probe_pipe::BRANCH, 1u
    };
    check(clock.issue(branch, &at) && at == 0u &&
          clock.reserve_annulled_delay_slot() &&
          clock.issue(b, &at) && at == 2u &&
          clock.instructions_issued() == 2u &&
          clock.annulled_slots_reserved() == 1u,
          "annulled branch-likely slot reserves timing without execution");

    clock.reset();
    check(clock.issue(branch) &&
          clock.issue(a, &at) && at == 1u &&
          clock.annulled_slots_reserved() == 0u,
          "executed delay instruction remains an ordinary issued instruction");

    clock.reset();
    check(clock.issue(branch) &&
          clock.charge_hle_overhead(300u) &&
          clock.issue(a, &at) && at == 301u &&
          clock.configured_hle_overhead() == 300u &&
          clock.annulled_slots_reserved() == 0u,
          "configured HLE overhead is separate from issue and annul slots");

    check(clock.estimated_cycles() == 302u &&
          !clock.probe_budget_reached(0u) &&
          !clock.probe_budget_reached(303u) &&
          clock.probe_budget_reached(302u),
          "probe budget is a read-only diagnostic, never a VBlank producer");

    clock.reset();
    check(!clock.issue(s_ee_probe_instruction{
              0u, 1u << 3u, e_ee_probe_pipe::INTEGER, 0u
          }) &&
          clock.estimated_cycles() == 0u &&
          clock.instructions_issued() == 0u,
          "invalid zero-result-latency instruction is atomic");

    check(clock.issue(s_ee_probe_instruction{
              0u, 1u << 3u, e_ee_probe_pipe::INTEGER, 10u
          }) &&
          clock.issue(s_ee_probe_instruction{
              1u << 3u, 0u, e_ee_probe_pipe::INTEGER, 1u
          }, &at) && at == 10u,
          "scoreboard observes explicitly provided long latency");

    clock.reset();
    check(clock.issue(a) &&
          clock.charge_hle_overhead(
              std::numeric_limits<uint32_t>::max()) &&
          clock.configured_hle_overhead() ==
              std::numeric_limits<uint32_t>::max(),
          "large HLE diagnostic costs preserve 64-bit accounting");
    clock.reset();
    check(clock.estimated_cycles() == 0u &&
          clock.instructions_issued() == 0u &&
          clock.configured_hle_overhead() == 0u,
          "reset clears all probe state");

    std::printf("ee_issue_timing_probe_test: %d failure(s)\n", g_failures);
    return g_failures != 0 ? 1 : 0;
}
