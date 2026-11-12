#include "ee_runtime.h"
#include "../../tools/vu0_diff/vu0_trace_fixture.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

namespace
{

uint32_t read_le32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t architectural_vi(const ee::s_ee_vu0_state& vu0, uint32_t index)
{
    switch (index)
    {
    case 16: return vu0.m_status_flags;
    case 17: return vu0.m_mac_flags;
    case 18: return vu0.m_clip_flags;
    case 26: return vu0.m_micro_pc;
    case 28: return vu0.m_fbrst;
    case 29: return vu0.m_vpu_status;
    default: return vu0.m_vi[index];
    }
}

void write_state(std::ostream& output, const ee::s_ee_vu0_state& vu0)
{
    output << "\"vi\":[";
    for (uint32_t index = 0; index < 32; ++index)
    {
        if (index != 0)
            output << ',';
        output << architectural_vi(vu0, index);
    }

    output << "],\"cycle\":" << vu0.m_cycle
           << ",\"micro_pc\":" << vu0.m_micro_pc
           << ",\"tpc\":" << vu0.m_tpc
           << ",\"branch\":" << vu0.m_branch
           << ",\"branch_pc\":" << vu0.m_branch_pc
           << ",\"delay_branch_pc\":" << vu0.m_delay_branch_pc
           << ",\"take_delay_branch\":"
           << (vu0.m_take_delay_branch != 0 ? "true" : "false")
           << ",\"vi_backup_cycles\":" << vu0.m_vi_backup_cycles
           << ",\"vi_backup_register\":" << vu0.m_vi_backup_register
           << ",\"vi_backup_old_value\":" << vu0.m_vi_backup_old_value
           << ",\"e_bit\":" << vu0.m_e_bit
           << ",\"m_bit\":" << (vu0.m_m_bit != 0 ? "true" : "false")
           << ",\"vpu_status\":" << vu0.m_vpu_status
           << ",\"running\":" << (vu0.m_running != 0 ? "true" : "false");
}

void write_header(std::ostream& output, const ee::s_ee_vu0_state& vu0)
{
    output << "{\"schema\":\"mips-to-llvm-vu0-trace-v1\""
           << ",\"kind\":\"header\""
           << ",\"engine\":\"mips-to-llvm-runtime\""
           << ",\"revision\":\"main\""
           << ",\"program_sha256\":\""
           << vu0_trace_fixture::k_program_sha256 << "\""
           << ",\"initial_state\":{";
    write_state(output, vu0);
    output << "}}\n";
}

void write_step(
    std::ostream& output,
    uint32_t step,
    uint32_t pair_pc,
    uint32_t lower,
    uint32_t upper,
    const ee::s_ee_vu0_state& vu0
)
{
    output << "{\"kind\":\"step\",\"step\":" << step
           << ",\"pc\":" << pair_pc
           << ",\"lower\":" << lower
           << ",\"upper\":" << upper
           << ",\"state\":{";
    write_state(output, vu0);
    output << "}}\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <output.jsonl>\n", argv[0]);
        return 2;
    }

    std::ofstream output(argv[1], std::ios::out | std::ios::trunc);
    if (!output)
    {
        std::fprintf(stderr, "cannot open trace output: %s\n", argv[1]);
        return 2;
    }

    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::s_ee_vu0_ialu_engine engine{};

    vu0_trace_fixture::install_program(
        vu0.m_micro_memory, ee::EE_VU0_MICRO_MEMORY_SIZE
    );
    vu0.m_vi[0] = 0;
    vu0.m_vpu_status = 1;
    vu0.m_vi[29] = vu0.m_vpu_status;
    vu0.m_running = 1;

    write_header(output, vu0);

    uint32_t step = 0;
    while ((vu0.m_vpu_status & 1u) != 0 &&
           step < vu0_trace_fixture::k_step_limit)
    {
        const uint32_t pair_pc = vu0.m_micro_pc &
            ee::EE_VU0_MICRO_MEMORY_MASK;
        const uint32_t lower = read_le32(&vu0.m_micro_memory[pair_pc]);
        const uint32_t upper = read_le32(&vu0.m_micro_memory[pair_pc + 4u]);
        const uint64_t previous_cycle = vu0.m_cycle;

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &engine,
            &cpu,
            &vu0,
            1,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES,
            0
        );
        if (result != static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE))
        {
            std::fprintf(
                stderr,
                "VU0 candidate failed at step %u (pc=0x%03x, result=%u)\n",
                step, pair_pc, result
            );
            return 1;
        }

        if (vu0.m_cycle <= previous_cycle)
        {
            std::fprintf(stderr, "VU0 candidate made no cycle progress at step %u\n", step);
            return 1;
        }

        write_step(output, step, pair_pc, lower, upper, vu0);
        ++step;
    }

    if ((vu0.m_vpu_status & 1u) != 0)
    {
        std::fprintf(
            stderr,
            "VU0 candidate exceeded the %u-pair trace limit\n",
            vu0_trace_fixture::k_step_limit
        );
        return 1;
    }

    if (step != vu0_trace_fixture::k_expected_step_count)
    {
        std::fprintf(
            stderr,
            "VU0 candidate executed %u pairs; expected %u\n",
            step, vu0_trace_fixture::k_expected_step_count
        );
        return 1;
    }

    output.flush();
    if (!output)
    {
        std::fprintf(stderr, "failed while writing trace: %s\n", argv[1]);
        return 1;
    }

    std::printf(
        "VU0 candidate trace: %u pairs, %llu cycles, final micro-PC 0x%03x\n",
        step,
        static_cast<unsigned long long>(vu0.m_cycle),
        vu0.m_micro_pc
    );
    return 0;
}
