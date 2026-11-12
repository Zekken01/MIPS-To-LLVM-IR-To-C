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

    std::printf("vu0_runtime_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
