#include "ee_exceptions.h"
#include "native_codegen.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int s_failures = 0;

void check(bool condition, const char* description)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", description);
        ++s_failures;
    }
}
}

int main()
{
    ee::s_ee_cpu_state cpu{};
    constexpr uint32_t k_timer_ip =
        1u << (ee::CAUSE_IP_SHIFT + ee::INT_TIMER);
    constexpr uint32_t k_intc_ip =
        1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC);
    constexpr uint32_t k_other_cause = k_intc_ip | 0x24u;
    constexpr uint32_t k_timer_enabled =
        ee::STATUS_IE | ee::STATUS_EIE | k_timer_ip;

    cpu.m_cop0[ee::COP0_Count] = 4u;
    cpu.m_cop0[ee::COP0_Compare] = 6u;
    cpu.m_cop0[ee::COP0_Status] = k_timer_enabled;
    cpu.m_cop0[ee::COP0_Cause] = k_other_cause | k_timer_ip;

    check(ee::ee_interrupts_enabled(&cpu),
          "timer pending and enabled before Compare write");

    ee::ee_cop0_write(&cpu, ee::COP0_Compare, 10u);
    check(cpu.m_cop0[ee::COP0_Compare] == 10u &&
          cpu.m_cop0[ee::COP0_Count] == 4u,
          "Compare write stores the value without advancing Count");
    check(cpu.m_cop0[ee::COP0_Cause] == k_other_cause &&
          !ee::ee_interrupts_enabled(&cpu),
          "Compare write clears only Cause.IP7 and masks timer delivery");

    ee::ee_cop0_write(&cpu, ee::COP0_Compare, 10u);
    check(cpu.m_cop0[ee::COP0_Cause] == k_other_cause,
          "writing the same Compare value does not fabricate an interrupt");

    ee::ee_cop0_write(&cpu, ee::COP0_Count, 7u);
    check(cpu.m_cop0[ee::COP0_Count] == 7u &&
          cpu.m_cop0[ee::COP0_Cause] == k_other_cause,
          "Count write preserves other pending interrupt lines");

    ee::ee_cop0_write(&cpu, ee::COP0_Status, 0x00010001u);
    check(cpu.m_cop0[ee::COP0_Status] == 0x00010001u &&
          cpu.m_cop0[ee::COP0_Cause] == k_other_cause,
          "non-Compare writes retain prior register semantics");

    // The existing explicit timer-tick helper is only a synthetic fixture:
    // no native execution cycle source or GS scheduling is installed.
    cpu.m_cop0[ee::COP0_Count] = 4u;
    cpu.m_cop0[ee::COP0_Compare] = 6u;
    cpu.m_cop0[ee::COP0_Cause] = k_other_cause;
    ee::ee_timer_tick(&cpu, 2u);
    check(cpu.m_cop0[ee::COP0_Count] == 6u &&
          (cpu.m_cop0[ee::COP0_Cause] & k_timer_ip) != 0u,
          "explicit timer-tick fixture can set IP7");

    ee::ee_cop0_write(&cpu, ee::COP0_Compare, 6u);
    check(cpu.m_cop0[ee::COP0_Cause] == k_other_cause &&
          cpu.m_cop0[ee::COP0_Count] == 6u,
          "Compare write acknowledges even when Count equals Compare");

    const ee::s_ee_cpu_state before_invalid = cpu;
    ee::ee_cop0_write(&cpu, 32u, 0xffffffffu);
    ee::ee_cop0_write(&cpu, 0xffffffffu, 0xffffffffu);
    ee::ee_cop0_write(nullptr, ee::COP0_Compare, 0u);
    check(cpu.m_cop0[ee::COP0_Compare] ==
              before_invalid.m_cop0[ee::COP0_Compare] &&
          cpu.m_cop0[ee::COP0_Cause] ==
              before_invalid.m_cop0[ee::COP0_Cause],
          "invalid indices and null CPU do not mutate architectural state");

    // The less frequently used C/C++ emitter must obey the same ABI as
    // generated LLVM, including the callable body's CPU pointer substitution.
    ee::s_lifted_op mtc0{};
    mtc0.kind = ee::LOP_MTC0;
    mtc0.rt = 8u;
    mtc0.rd = ee::COP0_Compare;
    mtc0.pc = 0x00100000u;
    ee::native_codegen generator;
    const std::string callable = generator.emit_function_body(
        std::vector<ee::s_lifted_op>{mtc0},
        mtc0.pc, "cop0_callable"
    );
    check(callable.find("ee_cop0_write(cpu, 11,") != std::string::npos &&
          callable.find("cpu->m_cop0[11] =") == std::string::npos,
          "native callable emitter calls the shared COP0 write helper");

    std::printf("ee_cop0_write_test: %d failure(s)\n", s_failures);
    return s_failures != 0 ? 1 : 0;
}
