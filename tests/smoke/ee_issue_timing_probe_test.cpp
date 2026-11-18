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
          clock.issue(a, &at) && at == 0u &&
          clock.estimated_cycles() == 1u &&
          clock.annulled_slots_reserved() == 0u,
          "branch and independent executed delay ALU may dual issue");

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

    // MULT can write a GPR result immediately while reserving HI/LO for
    // a separately configured latency. MFLO must observe the long latency.
    clock.reset();
    s_ee_probe_instruction multiply{
        (1u << 4u) | (1u << 5u), 1u << 6u,
        e_ee_probe_pipe::MULT_DIV, 1u
    };
    multiply.m_hilo_write_mask = 0x3u;
    multiply.m_hilo_result_latency = 7u;
    s_ee_probe_instruction mflo{
        0u, 1u << 7u, e_ee_probe_pipe::INTEGER, 1u
    };
    mflo.m_hilo_read_mask = 0x2u;
    check(clock.issue(multiply, &at) && at == 0u &&
          clock.issue(mflo, &at) && at == 7u &&
          clock.estimated_cycles() == 8u,
          "HI/LO dependency waits for caller-supplied result latency");

    clock.reset();
    s_ee_probe_instruction fadd{};
    fadd.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    fadd.m_fpr_read_mask = (1u << 1u) | (1u << 2u);
    fadd.m_fpr_write_mask = 1u << 3u;
    fadd.m_fpr_result_latency = 4u;
    s_ee_probe_instruction fconsumer{};
    fconsumer.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    fconsumer.m_fpr_read_mask = 1u << 3u;
    check(clock.issue(fadd, &at) && at == 0u &&
          clock.issue(fconsumer, &at) && at == 4u,
          "FPR scoreboard tracks explicit producer-to-consumer latency");

    clock.reset();
    s_ee_probe_instruction acc_writer{};
    acc_writer.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    acc_writer.m_acc_write = true;
    acc_writer.m_acc_result_latency = 6u;
    s_ee_probe_instruction acc_reader{};
    acc_reader.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    acc_reader.m_acc_read = true;
    check(clock.issue(acc_writer, &at) && at == 0u &&
          clock.issue(acc_reader, &at) && at == 6u,
          "FPU accumulator hazards use their own readiness clock");

    clock.reset();
    s_ee_probe_instruction invalid_hilo{};
    invalid_hilo.m_hilo_read_mask = 0x10u;
    check(!clock.issue(invalid_hilo) &&
          clock.instructions_issued() == 0u &&
          clock.estimated_cycles() == 0u,
          "invalid special-register masks are rejected atomically");

    clock.reset();
    s_ee_probe_instruction indep_hilo{};
    indep_hilo.m_hilo_write_mask = 0x1u;
    s_ee_probe_instruction dependent_hilo{};
    dependent_hilo.m_hilo_read_mask = 0x1u;
    check(clock.issue(indep_hilo, &at) && at == 0u &&
          clock.issue(dependent_hilo, &at) && at == 1u,
          "HI/LO same-cycle pairing observes RAW hazards");

    // Source: EE Core User's Manual v6.0, Tables 1-2 and 1-3.
    // All timings below remain synthetic candidates.
    clock.reset();
    check(clock.issue(load, &at) && at == 0u &&
          clock.issue(b, &at) && at == 0u &&
          clock.estimated_cycles() == 1u,
          "Pipe1 load and independent Pipe0 ALU can dual issue");

    clock.reset();
    s_ee_probe_instruction mac0{};
    mac0.m_pipe = e_ee_probe_pipe::MULT_DIV;
    s_ee_probe_instruction mac1{};
    mac1.m_pipe = e_ee_probe_pipe::MULT_DIV_1;
    check(clock.issue(mac0, &at) && at == 0u &&
          clock.issue(mac1, &at) && at == 0u,
          "independent MAC0 and MAC1 can use both logical pipes");

    clock.reset();
    check(clock.issue(branch, &at) && at == 0u &&
          clock.issue(branch, &at) && at == 1u,
          "Table 1-3 excludes branch plus branch pairing");

    clock.reset();
    s_ee_probe_instruction fpu_op{};
    fpu_op.m_pipe = e_ee_probe_pipe::COPROCESSOR_OP;
    s_ee_probe_instruction fpu_move{};
    fpu_move.m_pipe = e_ee_probe_pipe::COPROCESSOR_MOVE;
    check(clock.issue(fpu_op, &at) && at == 0u &&
          clock.issue(fpu_move, &at) && at == 1u,
          "COP1 operate/move Y resource-stall pairing stays unmodeled");

    clock.reset();
    s_ee_probe_instruction invalid_pipe{};
    invalid_pipe.m_pipe = static_cast<e_ee_probe_pipe>(255u);
    check(!clock.issue(invalid_pipe) && clock.instructions_issued() == 0u,
          "unrecognized issue class fails atomically");

    // An outstanding write also reserves its architectural destination.
    // Conservative WAW ordering applies across issue cycles and to FCR31.
    clock.reset();
    s_ee_probe_instruction long_gpr{};
    long_gpr.m_gpr_write_mask = 1u << 9u;
    long_gpr.m_result_latency = 8u;
    s_ee_probe_instruction replace_gpr{};
    replace_gpr.m_gpr_write_mask = 1u << 9u;
    check(clock.issue(long_gpr, &at) && at == 0u &&
          clock.issue(replace_gpr, &at) && at == 8u &&
          clock.estimated_cycles() == 9u,
          "long-latency GPR WAW is ordered across cycles");

    clock.reset();
    s_ee_probe_instruction fcsr_writer{};
    fcsr_writer.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    fcsr_writer.m_fcsr_write = true;
    fcsr_writer.m_fcsr_result_latency = 6u;
    s_ee_probe_instruction fcsr_reader{};
    fcsr_reader.m_pipe = e_ee_probe_pipe::BRANCH;
    fcsr_reader.m_fcsr_read = true;
    check(clock.issue(fcsr_writer, &at) && at == 0u &&
          clock.issue(fcsr_reader, &at) && at == 6u &&
          clock.estimated_cycles() == 7u,
          "FCR31 compare-to-condition-branch RAW waits for availability");

    clock.reset();
    check(clock.issue(fcsr_writer, &at) && at == 0u &&
          clock.issue(fcsr_writer, &at) && at == 6u &&
          clock.estimated_cycles() == 7u,
          "FCR31 WAW cannot reorder pending condition/control writes");

    clock.reset();
    check(clock.issue(fcsr_reader, &at) && at == 0u &&
          clock.issue(fcsr_writer, &at) && at == 1u,
          "FCR31 read/write do not pair in the same probe cycle");

    std::printf("ee_issue_timing_probe_test: %d failure(s)\n", g_failures);
    return g_failures != 0 ? 1 : 0;
}
