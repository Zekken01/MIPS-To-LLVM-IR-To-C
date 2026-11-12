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

    std::printf("vu0_runtime_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
