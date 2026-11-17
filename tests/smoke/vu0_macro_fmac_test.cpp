#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
int g_failures = 0;

void check(bool condition, const char* description)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

uint32_t bits(float value)
{
    uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

ee::s_ee_u128 make_vector(float x, float y, float z, float w)
{
    return ee::s_ee_u128{
        static_cast<uint64_t>(bits(x)) | (static_cast<uint64_t>(bits(y)) << 32),
        static_cast<uint64_t>(bits(z)) | (static_cast<uint64_t>(bits(w)) << 32)
    };
}

uint32_t lane(const ee::s_ee_u128& value, uint32_t index)
{
    const uint64_t half = index < 2 ? value.m_lo : value.m_hi;
    return static_cast<uint32_t>(half >> (32 * (index & 1)));
}

uint32_t sync_halted(
    void*, ee::s_ee_cpu_state*, ee::s_ee_vu0_state* vu0,
    uint32_t mode, uint32_t
)
{
    return mode == static_cast<uint32_t>(ee::e_ee_vu0_sync_mode::FINISH_TO_E) &&
           vu0->m_running == 0 && (vu0->m_vpu_status & 1u) == 0
        ? static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE)
        : static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
}

constexpr uint32_t s_matrix_words[16] =
{
    0x4be821bcu, 0x4be828bdu, 0x4be830beu, 0x4be83a0bu,
    0x4be921bcu, 0x4be928bdu, 0x4be930beu, 0x4be93a4bu,
    0x4bea21bcu, 0x4bea28bdu, 0x4bea30beu, 0x4bea3a8bu,
    0x4beb21bcu, 0x4beb28bdu, 0x4beb30beu, 0x4beb3acbu,
};

bool same_vector(const ee::s_ee_u128& a, const ee::s_ee_u128& b)
{
    return a.m_lo == b.m_lo && a.m_hi == b.m_hi;
}
} // namespace

int main()
{
    using ee::e_ee_vu0_sync_result;

    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::s_ee_runtime runtime{};
    runtime.m_vu0_state = &vu0;
    runtime.m_vu0_sync = sync_halted;

    // Original US game sequence, executing a four-column matrix transform.
    // The columns of an identity matrix must preserve every source vector.
    cpu.m_vu0[4] = make_vector(1.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[5] = make_vector(0.f, 1.f, 0.f, 0.f);
    cpu.m_vu0[6] = make_vector(0.f, 0.f, 1.f, 0.f);
    cpu.m_vu0[7] = make_vector(0.f, 0.f, 0.f, 1.f);
    cpu.m_vu0[8] = make_vector(2.f, 3.f, 4.f, 1.f);
    cpu.m_vu0[9] = make_vector(-1.f, 5.f, 7.f, 1.f);
    cpu.m_vu0[10] = make_vector(10.f, -8.f, 6.f, 1.f);
    cpu.m_vu0[11] = make_vector(1.f, 2.f, 3.f, 1.f);
    const ee::s_ee_u128 expected[4] = {
        cpu.m_vu0[8], cpu.m_vu0[9], cpu.m_vu0[10], cpu.m_vu0[11]
    };
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t result = ee::ee_vu0_macro_fmac(
            &cpu, &runtime, s_matrix_words[i], 0x00108240u + 4u * i
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "one authentic SDBZ macro executes and synchronizes halted VU0");
        if ((i & 3u) == 3u)
        {
            check(same_vector(cpu.m_vu0[8u + (i / 4u)], expected[i / 4u]),
                  "original SDBZ matrix chain preserves identity transform");
        }
    }

    // VMULAx initializes ACC, VMADDAy/z read it, and VMADDw writes VF
    // without overwriting ACC. Check the destination and negative MAC sign.
    cpu.m_vu0[4] = make_vector(-2.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[8] = make_vector(3.f, 0.f, 0.f, 0.f);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, s_matrix_words[0], 0x108240) == 0 &&
          lane(vu0.m_acc, 0) == bits(-6.f) &&
          (vu0.m_mac_flags & 0x80u) != 0,
          "VMULAx initializes ACC and records signed X MAC flag");

    // Masked-off lanes retain their ACC or VF payload, but clear MAC lanes.
    vu0.m_acc = make_vector(9.f, 8.f, 7.f, 6.f);
    vu0.m_mac_flags = 0xffffu;
    const uint32_t x_only = (s_matrix_words[0] & ~(15u << 21)) | (8u << 21);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          lane(vu0.m_acc, 0) == bits(-6.f) &&
          lane(vu0.m_acc, 1) == bits(8.f) &&
          lane(vu0.m_acc, 2) == bits(7.f) &&
          lane(vu0.m_acc, 3) == bits(6.f) &&
          (vu0.m_mac_flags & 0x7777u) == 0,
          "lane mask preserves unmapped ACC lanes and clears their MAC flags");

    // A VF0 destination discards writes but still updates flags. The
    // physical backing of VF0 must be ignored for reads and remain unchanged.
    cpu.m_vu0[0] = make_vector(21.f, 22.f, 23.f, 24.f);
    const ee::s_ee_u128 vf0_before = cpu.m_vu0[0];
    const uint32_t to_vf0 = s_matrix_words[3] & ~(31u << 6);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, to_vf0, 0x10824c) == 0 &&
          same_vector(cpu.m_vu0[0], vf0_before),
          "VMADDw discards VF0 destination without corrupting backing");

    // Unsupported instructions or a busy VU0 must fail closed. Confirm no
    // architectural writes, not merely a non-zero return value.
    const ee::s_ee_cpu_state cpu_before = cpu;
    const ee::s_ee_vu0_state vu0_before = vu0;
    const uint32_t unsupported = s_matrix_words[0] ^ 1u;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, unsupported, 0x108240) ==
              static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT) &&
          std::memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0 &&
          std::memcmp(&vu0, &vu0_before, sizeof(vu0)) == 0,
          "unsupported SPECIAL2 operation leaves CPU and VU0 unmodified");
    vu0.m_running = 1;
    const ee::s_ee_cpu_state busy_before = cpu;
    const ee::s_ee_vu0_state busy_vu0 = vu0;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, s_matrix_words[0], 0x108240) ==
              static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
          std::memcmp(&cpu, &busy_before, sizeof(cpu)) == 0 &&
          std::memcmp(&vu0, &busy_vu0, sizeof(vu0)) == 0,
          "running VU0 without a finishing microengine cannot be bypassed");
    vu0.m_running = 0;
    runtime.m_vu0_sync = nullptr;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, s_matrix_words[0], 0x108240) ==
              static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
          "missing VU0 synchronization callback cannot succeed");

    std::printf("vu0_macro_fmac_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
