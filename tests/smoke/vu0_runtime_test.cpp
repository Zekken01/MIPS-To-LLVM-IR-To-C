#include "ee_runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace
{

struct s_mock_scheduler
{
    uint32_t m_calls = 0;
    uint32_t m_last_mode = 0xffffffffu;
    uint32_t m_last_pc = 0;
    ee::s_ee_cpu_state* m_last_cpu = nullptr;
    ee::s_ee_vu0_state* m_last_vu0 = nullptr;
    uint32_t m_result = static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
};

uint32_t mock_vu0_sync(
    void* user,
    ee::s_ee_cpu_state* cpu,
    ee::s_ee_vu0_state* vu0,
    uint32_t mode,
    uint32_t pc
)
{
    auto* scheduler = static_cast<s_mock_scheduler*>(user);
    if (!scheduler)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }

    ++scheduler->m_calls;
    scheduler->m_last_mode = mode;
    scheduler->m_last_pc = pc;
    scheduler->m_last_cpu = cpu;
    scheduler->m_last_vu0 = vu0;

    // A test scheduler mutation proves that the callback is operating on the
    // exact state objects owned by the caller, not copies or hidden globals.
    vu0->m_cycle += 7;
    cpu->m_pc = pc;
    return scheduler->m_result;
}

struct s_mock_control
{
    uint32_t m_calls = 0;
    uint32_t m_last_register = 0xffffffffu;
    uint32_t m_last_value = 0;
    uint32_t m_last_pc = 0;
    uint32_t m_result = static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
};

uint32_t mock_vu0_control(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t register_index,
    uint32_t value,
    uint32_t pc
)
{
    auto* control = static_cast<s_mock_control*>(user);
    if (!control)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }

    ++control->m_calls;
    control->m_last_register = register_index;
    control->m_last_value = value;
    control->m_last_pc = pc;
    return control->m_result;
}

struct s_mock_vu0_engine
{
    uint32_t m_run_calls = 0;
    uint32_t m_vu1_calls = 0;
    uint32_t m_event_calls = 0;
    uint64_t m_last_budget = 0;
    uint32_t m_last_flags = 0;
    uint32_t m_last_pc = 0;
    uint32_t m_last_event_delta = 0;
    uint32_t m_clear_e_after_call = 0;
    uint32_t m_set_m_after_call = 0;
    uint64_t m_cycles_per_call = 10;
    uint32_t m_execute_result =
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
    uint32_t m_vu1_result =
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
};

uint32_t mock_vu0_execute(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state* vu0,
    uint64_t max_cycles,
    uint32_t flags,
    uint32_t pc
)
{
    auto* engine = static_cast<s_mock_vu0_engine*>(user);
    if (!engine || !vu0)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }

    ++engine->m_run_calls;
    engine->m_last_budget = max_cycles;
    engine->m_last_flags = flags;
    engine->m_last_pc = pc;
    if (engine->m_execute_result !=
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE))
    {
        return engine->m_execute_result;
    }

    const bool exact = (flags & ee::EE_VU0_EXECUTE_EXACT_CYCLES) != 0;
    vu0->m_cycle += exact ? max_cycles : engine->m_cycles_per_call;
    if (engine->m_set_m_after_call != 0 &&
        engine->m_run_calls >= engine->m_set_m_after_call)
    {
        vu0->m_m_bit = 1;
    }
    if (engine->m_clear_e_after_call != 0 &&
        engine->m_run_calls >= engine->m_clear_e_after_call)
    {
        vu0->m_vpu_status &= ~1u;
    }
    return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
}

uint32_t mock_vu1_catch_up(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t
)
{
    auto* engine = static_cast<s_mock_vu0_engine*>(user);
    if (!engine)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }
    ++engine->m_vu1_calls;
    return engine->m_vu1_result;
}

void mock_vu0_schedule_event(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t delta_cycles,
    uint32_t pc
)
{
    auto* engine = static_cast<s_mock_vu0_engine*>(user);
    if (!engine) return;
    ++engine->m_event_calls;
    engine->m_last_event_delta = delta_cycles;
    engine->m_last_pc = pc;
}

ee::s_ee_vu0_scheduler make_mock_vu0_scheduler(s_mock_vu0_engine* engine)
{
    ee::s_ee_vu0_scheduler scheduler{};
    scheduler.m_user = engine;
    scheduler.m_execute = mock_vu0_execute;
    scheduler.m_vu1_catch_up = mock_vu1_catch_up;
    scheduler.m_schedule_event = mock_vu0_schedule_event;
    scheduler.m_max_run_calls = 16;
    return scheduler;
}

int g_pass = 0;
int g_fail = 0;

void check(bool condition, const char* name)
{
    if (condition)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("FAIL %s\n", name);
    }
}

} // namespace

static_assert(std::is_standard_layout_v<ee::s_ee_vu0_state>);
static_assert(alignof(ee::s_ee_vu0_state) == 16);
static_assert(offsetof(ee::s_ee_vu0_state, m_vi) == 0);
static_assert(offsetof(ee::s_ee_vu0_state, m_acc) == 128);
static_assert(offsetof(ee::s_ee_vu0_state, m_q) == 144);
static_assert(offsetof(ee::s_ee_vu0_state, m_p) == 160);
static_assert(offsetof(ee::s_ee_vu0_state, m_cycle) == 208);
static_assert(offsetof(ee::s_ee_vu0_state, m_ee_cycle) == 216);
static_assert(offsetof(ee::s_ee_vu0_state, m_running) == 224);
static_assert(sizeof(ee::s_ee_vu0_state) == 240);

int main()
{
    using ee::e_ee_vu0_sync_mode;
    using ee::e_ee_vu0_sync_result;
    using ee::e_ee_vu0_transfer_direction;

    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    s_mock_scheduler scheduler{};

    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::FROM_VU0, false
        ) == e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE,
        "read transfer outside delay slot catches VU0 up to EE cycle"
    );
    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::TO_VU0, false
        ) == e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE,
        "write transfer outside delay slot catches VU0 up to EE cycle"
    );
    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::FROM_VU0, true
        ) == e_ee_vu0_sync_mode::FINISH_TO_E,
        "read transfer in delay slot finishes to E"
    );
    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::TO_VU0, true
        ) == e_ee_vu0_sync_mode::WAIT_TO_E_OR_M,
        "write transfer in delay slot waits to E or M"
    );

    vu0.m_vi[0] = 0xffffffffu;
    vu0.m_vi[7] = 0x81234567u;
    check(ee::ee_vu0_read_vi(&vu0, 0) == 0,
          "VI0 reads as the hardwired zero register");
    check(ee::ee_vu0_read_vi(&vu0, 7) == 0x81234567u,
          "VI register read returns the stored word");
    ee::ee_vu0_write_vi(&vu0, 0, 0x12345678u);
    ee::ee_vu0_write_vi(&vu0, 7, 0x76543210u);
    check(vu0.m_vi[0] == 0xffffffffu,
          "VI0 writes are ignored");
    check(ee::ee_vu0_read_vi(&vu0, 7) == 0x76543210u,
          "VI register write updates the selected register");
    check(ee::ee_vu0_read_vi(&vu0, 32) == 0,
          "out-of-range VI read fails closed");
    ee::ee_vu0_write_vi(&vu0, 32, 0xdeadbeefu);
    check(vu0.m_vi[7] == 0x76543210u,
          "out-of-range VI write is ignored");

    ee::s_ee_runtime runtime{};
    runtime.m_vu0_state = &vu0;
    runtime.m_vu0_user = &scheduler;
    runtime.m_vu0_sync = mock_vu0_sync;
    s_mock_control control{};
    runtime.m_vu0_control_user = &control;
    runtime.m_vu0_control = mock_vu0_control;

    const uint32_t modes[] = {
        static_cast<uint32_t>(e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
        static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
        static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M)
    };
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t pc = 0x00100000u + (i * 4);
        const uint32_t result = ee::ee_vu0_sync(&cpu, &runtime, modes[i], pc);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "scheduler completion propagates");
        check(scheduler.m_last_mode == modes[i],
              "scheduler receives requested synchronization mode");
        check(scheduler.m_last_pc == pc,
              "scheduler receives the guest instruction PC");
        check(scheduler.m_last_cpu == &cpu && scheduler.m_last_vu0 == &vu0,
              "scheduler receives the original CPU and VU0 state");
    }
    check(scheduler.m_calls == 3, "all three synchronization modes dispatched");
    check(vu0.m_cycle == 21, "scheduler updates the VU0 cycle state");
    check(cpu.m_pc == 0x00100008u, "scheduler may update the EE state");

    scheduler.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    check(
        ee::ee_vu0_sync(&cpu, &runtime, modes[0], 0x0010000cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "scheduler failure propagates"
    );
    scheduler.m_result = 0xffffffffu;
    check(
        ee::ee_vu0_sync(&cpu, &runtime, modes[0], 0x00100010u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "unknown scheduler result is converted to failure"
    );

    ee::s_ee_runtime missing_callback{};
    missing_callback.m_vu0_state = &vu0;
    check(
        ee::ee_vu0_sync(&cpu, &missing_callback, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "missing scheduler fails closed"
    );

    ee::s_ee_runtime missing_state{};
    missing_state.m_vu0_sync = mock_vu0_sync;
    missing_state.m_vu0_user = &scheduler;
    check(
        ee::ee_vu0_sync(&cpu, &missing_state, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "missing VU0 state fails closed"
    );
    check(
        ee::ee_vu0_sync(nullptr, &runtime, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
        "null CPU is rejected"
    );
    check(
        ee::ee_vu0_sync(&cpu, nullptr, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
        "null runtime is rejected"
    );
    check(
        ee::ee_vu0_sync(&cpu, &runtime, 3, 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
        "unknown synchronization mode is rejected"
    );

    scheduler.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);

    // QMFC2 copies both halves of VF[fs] after synchronization.
    cpu.m_vu0[3] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    cpu.m_gpr[9] = {};
    const uint32_t read_mode = static_cast<uint32_t>(
        ee::ee_vu0_transfer_sync_mode(e_ee_vu0_transfer_direction::FROM_VU0, false)
    );
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 9, 3, read_mode, 0x00100020u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMFC2 succeeds after scheduler synchronization"
    );
    check(cpu.m_gpr[9].m_lo == cpu.m_vu0[3].m_lo &&
          cpu.m_gpr[9].m_hi == cpu.m_vu0[3].m_hi,
          "QMFC2 copies both 64-bit halves");
    const ee::s_ee_u128 zero_gpr_sentinel{0x1111222233334444ULL, 0x5555666677778888ULL};
    cpu.m_gpr[0] = zero_gpr_sentinel;
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 0, 3, read_mode, 0x00100024u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMFC2 to GPR0 still performs synchronization"
    );
    check(cpu.m_gpr[0].m_lo == zero_gpr_sentinel.m_lo &&
          cpu.m_gpr[0].m_hi == zero_gpr_sentinel.m_hi,
          "QMFC2 never changes GPR0");

    // QMTC2 copies all 128 source bits. GPR0 reads as zero and VF0 is
    // protected from writes; synchronization still happens in both cases.
    cpu.m_gpr[8] = {0x8877665544332211ULL, 0x0123456789abcdefULL};
    const uint32_t write_mode = static_cast<uint32_t>(
        ee::ee_vu0_transfer_sync_mode(e_ee_vu0_transfer_direction::TO_VU0, false)
    );
    check(
        ee::ee_vu0_qmtc2(&cpu, &runtime, 8, 4, write_mode, 0x00100028u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMTC2 succeeds after scheduler synchronization"
    );
    check(cpu.m_vu0[4].m_lo == cpu.m_gpr[8].m_lo &&
          cpu.m_vu0[4].m_hi == cpu.m_gpr[8].m_hi,
          "QMTC2 copies both 64-bit halves");
    cpu.m_vu0[5] = {0xaaaaaaaa55555555ULL, 0xcccccccc33333333ULL};
    check(
        ee::ee_vu0_qmtc2(&cpu, &runtime, 0, 5, write_mode, 0x0010002cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMTC2 from GPR0 succeeds"
    );
    check(cpu.m_vu0[5].m_lo == 0 && cpu.m_vu0[5].m_hi == 0,
          "QMTC2 treats GPR0 as zero");
    const ee::s_ee_u128 zero_vf_sentinel{0xdeadbeef01234567ULL, 0x89abcdef76543210ULL};
    cpu.m_vu0[0] = zero_vf_sentinel;
    check(
        ee::ee_vu0_qmtc2(&cpu, &runtime, 8, 0, write_mode, 0x00100030u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMTC2 to VF0 still performs synchronization"
    );
    check(cpu.m_vu0[0].m_lo == zero_vf_sentinel.m_lo &&
          cpu.m_vu0[0].m_hi == zero_vf_sentinel.m_hi,
          "QMTC2 never changes VF0");
    cpu.m_gpr[13] = {};
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 13, 0, read_mode, 0x00100032u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMFC2 from VF0 succeeds"
    );
    check(cpu.m_gpr[13].m_lo == 0 &&
          cpu.m_gpr[13].m_hi == 0x3f80000000000000ULL,
          "QMFC2 reads VF0 as (0, 0, 0, 1.0f)");

    // CFC2 sign-extends ordinary VI words; R only replaces UL[0].
    vu0.m_vi[6] = 0x81234567u;
    cpu.m_gpr[10].m_hi = 0x123456789abcdef0ULL;
    check(
        ee::ee_vu0_cfc2(&cpu, &runtime, 10, 6, read_mode, 0x00100034u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CFC2 succeeds after scheduler synchronization"
    );
    check(cpu.m_gpr[10].m_lo == 0xffffffff81234567ULL,
          "CFC2 sign-extends ordinary VI values");
    check(cpu.m_gpr[10].m_hi == 0x123456789abcdef0ULL,
          "CFC2 preserves the upper 64-bit GPR half");
    vu0.m_vi[20] = 0xffabcdefu;
    cpu.m_gpr[11].m_lo = 0xfeedfacecafebeefULL;
    check(
        ee::ee_vu0_cfc2(&cpu, &runtime, 11, 20, read_mode, 0x00100038u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CFC2 special R read succeeds"
    );
    check(cpu.m_gpr[11].m_lo == 0xfeedface002bcdefULL,
          "CFC2 R masks UL[0] and preserves UL[1]");
    cpu.m_gpr[0] = zero_gpr_sentinel;
    check(
        ee::ee_vu0_cfc2(&cpu, &runtime, 0, 6, read_mode, 0x0010003cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CFC2 to GPR0 still performs synchronization"
    );
    check(cpu.m_gpr[0].m_lo == zero_gpr_sentinel.m_lo &&
          cpu.m_gpr[0].m_hi == zero_gpr_sentinel.m_hi,
          "CFC2 never changes GPR0");

    // CFC2 reads the dedicated flag/control fields, not stale VI array slots.
    vu0.m_mac_flags = 0x12345678u;
    vu0.m_clip_flags = 0x87654321u;
    vu0.m_status_flags = 0x0badc0deu;
    vu0.m_vpu_status = 0x00000005u;
    vu0.m_tpc = 0x00001234u;
    check(ee::ee_vu0_read_vi(&vu0, 17) == 0x12345678u,
          "CFC2 MAC flag reads tracked MAC flags");
    check(ee::ee_vu0_read_vi(&vu0, 18) == 0x87654321u,
          "CFC2 clip flag reads tracked clip flags");
    check(ee::ee_vu0_read_vi(&vu0, 16) == 0x0badc0deu,
          "CFC2 status flag reads tracked status flags");
    check(ee::ee_vu0_read_vi(&vu0, 29) == 0x00000005u,
          "CFC2 VPU status reads tracked VPU status");
    check(ee::ee_vu0_read_vi(&vu0, 26) == 0x00001234u,
          "CFC2 TPC reads tracked micro-PC");

    // Ordinary CTC2 writes VI, while the dedicated special cases are
    // implemented explicitly and the three read-only registers ignore writes.
    cpu.m_gpr[8].m_lo = 0x89abcdefu;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 7, write_mode, 0x00100048u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 ordinary VI write succeeds"
    );
    check(ee::ee_vu0_read_vi(&vu0, 7) == 0x89abcdefu,
          "CTC2 writes the selected VI register");
    vu0.m_mac_flags = 0x12345678u;
    cpu.m_gpr[8].m_lo = 0xffffffffu;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 17, write_mode, 0x0010004cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 MAC_FLAG read-only write is ignored"
    );
    check(vu0.m_mac_flags == 0x12345678u,
          "CTC2 cannot modify MAC_FLAG");
    vu0.m_tpc = 0x1234u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 26, write_mode, 0x00100050u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 TPC read-only write is ignored"
    );
    check(vu0.m_tpc == 0x1234u, "CTC2 cannot modify TPC");
    vu0.m_vpu_status = 5u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 29, write_mode, 0x00100054u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 VPU_STAT read-only write is ignored"
    );
    check(vu0.m_vpu_status == 5u, "CTC2 cannot modify VPU_STAT");

    cpu.m_gpr[8].m_lo = 0xabcdef01u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 20, write_mode, 0x00100058u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 R write succeeds"
    );
    check(ee::ee_vu0_read_vi(&vu0, 20) == ((0xabcdef01u & 0x007fffffu) | 0x3f800000u),
          "CTC2 R preserves the implicit 1.0 exponent");

    cpu.m_gpr[8].m_lo = 0x12345678u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 18, write_mode, 0x0010005cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 CLIP_FLAG write succeeds"
    );
    check(vu0.m_clip_flags == 0x12345678u &&
          ee::ee_vu0_read_vi(&vu0, 18) == 0x12345678u,
          "CTC2 CLIP_FLAG updates VI and the clip flag");

    cpu.m_gpr[8].m_lo = 0x00000c0fu;
    const uint32_t control_calls_before_fbrst = control.m_calls;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 28, write_mode, 0x00100060u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 FBRST host action succeeds"
    );
    check(control.m_calls == control_calls_before_fbrst + 1 &&
          control.m_last_register == 28 && control.m_last_value == 0x00000c0fu,
          "CTC2 FBRST forwards reset/break bits to the host");
    check(ee::ee_vu0_read_vi(&vu0, 28) == 0x00000c0cu,
          "CTC2 FBRST stores only the documented mask");

    cpu.m_gpr[8].m_lo = 0x12345678u;
    const uint32_t control_calls_before_cmsar = control.m_calls;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 31, write_mode, 0x00100064u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 CMSAR1 host action succeeds"
    );
    check(control.m_calls == control_calls_before_cmsar + 1 &&
          control.m_last_register == 31 && control.m_last_value == 0x5678u,
          "CTC2 CMSAR1 forwards its low 16-bit micro entry address");

    // Side-effectful writes fail closed without a host action callback.
    ee::s_ee_runtime no_control = runtime;
    no_control.m_vu0_control = nullptr;
    const uint32_t old_fbrst = vu0.m_fbrst;
    cpu.m_gpr[8].m_lo = 0x00000001u;
    check(
        ee::ee_vu0_ctc2(&cpu, &no_control, 8, 28, write_mode, 0x00100068u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "CTC2 FBRST fails closed without host action callback"
    );
    check(vu0.m_fbrst == old_fbrst,
          "failed FBRST action leaves register state unchanged");
    check(
        ee::ee_vu0_ctc2(&cpu, &no_control, 8, 31, write_mode, 0x0010006cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "CTC2 CMSAR1 fails closed without host action callback"
    );

    control.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    const uint32_t old_cmsar1 = vu0.m_cmsar1;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 31, write_mode, 0x00100070u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "CTC2 propagates host action failure"
    );
    check(vu0.m_cmsar1 == old_cmsar1,
          "failed CMSAR1 action leaves state unchanged");
    control.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);

    cpu.m_gpr[12] = {0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL};
    scheduler.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 12, 3, read_mode, 0x00100040u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "QMFC2 propagates scheduler failure"
    );
    check(cpu.m_gpr[12].m_lo == 0xaaaaaaaaaaaaaaaaULL &&
          cpu.m_gpr[12].m_hi == 0xbbbbbbbbbbbbbbbbULL,
          "failed QMFC2 leaves destination unchanged");

    // The concrete scheduler adapter applies the PCSX2 synchronization
    // policy around a host-provided VU execution callback.
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 30;
        state.m_ee_cycle = 100;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
            0x00101000u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "catch-up scheduler completes");
        check(engine.m_run_calls == 1 && engine.m_last_budget == 70,
              "catch-up executes exactly the EE/VU0 cycle difference");
        check((engine.m_last_flags & ee::EE_VU0_EXECUTE_EXACT_CYCLES) != 0 &&
              (engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_E) != 0 &&
              (engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_M) == 0,
              "catch-up requests exact cycles and ignores M-bit");
        check(state.m_cycle == 100 && state.m_ee_cycle == 100,
              "catch-up advances VU0 to EE without changing EE time");
        check(engine.m_vu1_calls == 0 && engine.m_event_calls == 0,
              "catch-up does not run the interlocked epilogue");
    }
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 101;
        state.m_ee_cycle = 100;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
            0x00101004u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 0,
              "catch-up does not run VU0 that is already ahead");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_clear_e_after_call = 2;
        engine.m_set_m_after_call = 1;
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00101008u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 2,
              "finish-to-E continues through an M-bit and stops at E");
        check((engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_M) == 0,
              "finish-to-E does not request M-bit termination");
        check(state.m_cycle == 40 && state.m_ee_cycle == 40,
              "finish-to-E accounts VU0 elapsed cycles on EE");
        check(engine.m_vu1_calls == 1 && engine.m_event_calls == 0,
              "finish-to-E catches VU1 up without scheduling a stopped VU0");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_set_m_after_call = 1;
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 100;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M),
            0x0010100cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 1,
              "wait-to-E-or-M stops on the first M-bit");
        check((engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_M) != 0,
              "wait-to-E-or-M requests M-bit termination");
        check(state.m_ee_cycle == state.m_cycle && state.m_cycle == 30,
              "wait-to-E-or-M catches EE time up to the VU0 cycle");
        check(engine.m_vu1_calls == 1 && engine.m_event_calls == 1 &&
              engine.m_last_event_delta == 4,
              "wait-to-E-or-M catches VU1 up and schedules a running VU0");
        check(engine.m_last_pc == 0x0010100cu,
              "VU1 catch-up and event scheduling retain the guest PC");
    }
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_m_bit = 1;
        state.m_cycle = 40;
        state.m_ee_cycle = 30;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M),
            0x00101010u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 0,
              "wait-to-E-or-M takes the early M-bit catch-up path");
        check(state.m_ee_cycle == 40 && engine.m_vu1_calls == 0 &&
              engine.m_event_calls == 0,
              "early M-bit path skips the normal VU1/event epilogue");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_execute_result =
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00101014u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED) &&
              engine.m_vu1_calls == 0 && engine.m_event_calls == 0,
              "VU0 engine failure stops before VU1/event callbacks");
    }
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        scheduler.m_vu1_catch_up = nullptr;
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00101018u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              engine.m_run_calls == 0,
              "finish-to-E fails closed before execution without VU1 catch-up");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_cycles_per_call = 1;
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        scheduler.m_max_run_calls = 2;
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x0010101cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED) &&
              engine.m_run_calls == 2,
              "finish-to-E enforces the configured execution-call budget");
    }

    std::printf("vu0_runtime_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
