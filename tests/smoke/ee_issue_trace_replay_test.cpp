#include "ee_issue_trace_replay.h"

#include <cstdio>

namespace
{
int s_failures = 0;
void check(bool success, const char* description)
{
    if (!success)
    {
        std::printf("FAIL: %s\n", description);
        ++s_failures;
    }
}
ee::s_ee_instruction_trace_entry record(
    uint64_t attempt, uint32_t pc, uint32_t opcode,
    uint32_t kind, uint64_t slot)
{
    ee::s_ee_instruction_trace_entry item{};
    item.m_attempt = attempt;
    item.m_pc = pc;
    item.m_opcode_word = opcode;
    item.m_kind = kind;
    item.m_reference_slot = slot;
    return item;
}
}

int main()
{
    using ee::c_ee_issue_trace_replay;
    using ee::e_ee_issue_replay_status;
    c_ee_issue_trace_replay replay{};

    check(replay.observe(record(1u, 0x1000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNCONFIGURED,
          "requires explicit latency profile");
    check(!replay.configure({0u, 3u}) && replay.configure({1u, 3u}),
          "rejects zero latency");

    check(replay.observe(record(1u, 0x1000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, 0x24090002u, 2u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 1u,
          "native and RAM independent instructions can pair");

    check(replay.observe(record(3u, 0x1008u, 0x8d0a0000u, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(4u, 0x100cu, 0x01495821u, 2u, 4u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 5u,
          "load stall persists across native/interpreted transition");

    constexpr uint32_t k_beql = (0x14u << 26u) | (11u << 21u) | 1u;
    check(replay.observe(record(5u, 0x1010u, k_beql, 0u, 5u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(5u, 0x1014u, 0x240c0001u, 1u, 6u)) ==
          e_ee_issue_replay_status::ANNULLED &&
          replay.observe(record(6u, 0x1018u, 0x240d0002u, 0u, 7u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 8u &&
          replay.executed_instructions() == 6u &&
          replay.annulled_slots() == 1u,
          "annulled delay slot reserves candidate time without execution");

    replay.configure({1u, 3u});
    check(replay.observe(record(100u, 0x2000u, 0x24080001u, 0u, 100u)) ==
          e_ee_issue_replay_status::INCOMPLETE_STREAM &&
          !replay.usable_prefix() &&
          replay.failure_pc() == 0x2000u &&
          replay.observe(record(1u, 0x1000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::HALTED,
          "filtered and incomplete streams fail closed");

    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, k_beql, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(1u, 0x1008u, 0x24090002u, 1u, 2u)) ==
          e_ee_issue_replay_status::INVALID_SLOT,
          "annulment requires exact preceding branch-likely delay address");

    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, 0x0000000cu, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "no fabricated HLE syscall latency");
    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, 0x00000018u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "no fabricated HI/LO pipeline cost");
    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, 0x46000000u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "no fabricated COP1 pipeline cost");

    std::printf("ee_issue_trace_replay_test: %d failure(s)\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
