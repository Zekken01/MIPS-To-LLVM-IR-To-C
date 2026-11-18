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

    // Per-pipeline availability is never inferred from the generic scalar
    // profile. Demand an explicit MUL, DIV, move or accumulate latency.
    replay.configure({1u, 3u});
    const uint32_t mult = (4u << 21u) | (5u << 16u) | (6u << 11u) | 0x18u;
    const uint32_t mflo = (7u << 11u) | 0x12u;
    check(replay.observe(record(1u, 0x1000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "HI/LO multiply is unavailable without calibrated latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    check(replay.observe(record(1u, 0x1000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, mflo, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 8u,
          "separate MULT HI0/LO0 latency delays MFLO without GPR aliasing");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t div1 = (4u << 21u) | (5u << 16u) |
                          (0x1cu << 26u) | 0x1au;
    const uint32_t mflo1 = (0x1cu << 26u) | (7u << 11u) | 0x12u;
    check(replay.observe(record(1u, 0x1000u, div1, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, mflo1, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 12u,
          "second HI1/LO1 divide pipeline gets an independent delay");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t madd1 = (0x1cu << 26u) | (4u << 21u) |
                           (5u << 16u) | 0x20u;
    check(replay.observe(record(1u, 0x1000u, madd1, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, mflo1, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 9u,
          "accumulation reads/writes HI1/LO1 with explicit latency");

    // COP1 FPR0 is not a hardwired zero register. Preserve hazards across
    // GPR-to-FPR moves, arithmetic and FPR-to-GPR moves.
    replay.configure({1u, 3u});
    const uint32_t mtc1 = (0x11u << 26u) | (4u << 21u) |
                          (8u << 16u) | (2u << 11u);
    check(replay.observe(record(1u, 0x1000u, mtc1, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "unconfigured COP1 transfer refuses guessed latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u, 2u, 5u, 14u, 9u});
    const uint32_t adds = (0x11u << 26u) | (0x10u << 21u) |
                          (3u << 16u) | (2u << 11u) | (4u << 6u);
    const uint32_t mfc1 = (0x11u << 26u) | (9u << 16u) | (4u << 11u);
    check(replay.observe(record(1u, 0x1000u, mtc1, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, adds, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(3u, 0x1008u, mfc1, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 8u,
          "MTC1 to ADD.S to MFC1 preserves FPR register dependencies");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u, 2u, 5u, 14u, 9u});
    const uint32_t adda = (0x11u << 26u) | (0x10u << 21u) |
                          (3u << 16u) | (2u << 11u) | 0x18u;
    const uint32_t madds = (0x11u << 26u) | (0x10u << 21u) |
                           (3u << 16u) | (2u << 11u) |
                           (4u << 6u) | 0x1cu;
    check(replay.observe(record(1u, 0x1000u, adda, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, madds, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 10u,
          "ADDA.S to MADD.S waits on independent FPU ACC scoreboard");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u, 2u, 5u, 14u, 9u});
    check(replay.observe(record(1u, 0x1000u, 0x45000001u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "FCSR-dependent BC1 remains unavailable");

    // Host-side HLE handoffs are not instructions or cycles. The service
    // identity is visible and the issue replay must refuse an unknown cost.
    ee::s_ee_runtime runtime{};
    ee::s_ee_instruction_trace_entry buffer[1]{};
    runtime.m_instruction_trace = buffer;
    runtime.m_instruction_trace_capacity = 1u;
    runtime.m_guest_instruction_attempts = 10u;
    runtime.m_reference_instruction_slots = 11u;
    ee::ee_record_hle_timing_boundary(runtime, 0x2000u, 0x0000000cu, 0x76u);
    check(runtime.m_hle_timing_boundaries == 1u &&
          runtime.m_guest_instruction_attempts == 10u &&
          runtime.m_reference_instruction_slots == 11u &&
          runtime.m_instruction_trace_count == 1u &&
          buffer[0].m_kind == 3u &&
          buffer[0].m_hle_service == 0x76u &&
          buffer[0].m_reference_slot == 11u,
          "HLE event preserves instruction and slot ordinals");

    ee::ee_record_hle_timing_boundary(runtime, 0x2000u, 0x0000000cu, 0x77u);
    check(runtime.m_hle_timing_boundaries == 2u &&
          runtime.m_instruction_trace_count == 1u &&
          runtime.m_instruction_trace_overflow == 1u,
          "HLE boundary on full trace is reported, not silently dropped");

    replay.configure({1u, 3u});
    check(replay.observe(buffer[0]) ==
          e_ee_issue_replay_status::UNMODELED_HLE_BOUNDARY &&
          !replay.usable_prefix(),
          "unknown HLE cost cannot produce a continuous candidate clock");

    std::printf("ee_issue_trace_replay_test: %d failure(s)\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
