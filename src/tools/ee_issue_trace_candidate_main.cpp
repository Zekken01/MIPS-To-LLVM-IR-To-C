#include "ee_issue_trace_replay.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
bool parse_latency(const char* text, uint32_t* value)
{
    if (text == nullptr || text[0] == '\0' || text[0] == '-')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed == 0u || parsed > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

struct s_candidate_event
{
    uint64_t m_slot = 0u;
    uint32_t m_pc = 0u;
    uint32_t m_opcode = 0u;
    uint32_t m_kind = 0u;
    uint64_t m_cycle = 0u;
};

bool parse_entry(const std::string& line,
                 ee::s_ee_instruction_trace_entry* entry)
{
    unsigned long long attempt = 0u;
    unsigned long long slot = 0u;
    unsigned int pc = 0u;
    unsigned int opcode = 0u;
    unsigned int kind = 0u;
    unsigned int service = 0u;
    int consumed = 0;
    const int parsed = std::sscanf(
        line.c_str(), "%llu,0x%x,0x%x,%u,%llu,%u%n",
        &attempt, &pc, &opcode, &kind, &slot, &service, &consumed
    );
    if (parsed != 6 || consumed != static_cast<int>(line.size()))
    {
        return false;
    }
    entry->m_attempt = static_cast<uint64_t>(attempt);
    entry->m_pc = static_cast<uint32_t>(pc);
    entry->m_opcode_word = static_cast<uint32_t>(opcode);
    entry->m_kind = static_cast<uint32_t>(kind);
    entry->m_reference_slot = static_cast<uint64_t>(slot);
    entry->m_hle_service = static_cast<uint32_t>(service);
    return true;
}
}

int main(int argc, char** argv)
{
    constexpr const char* k_output_option = "--candidate-events=";
    const std::string last_arg = argc > 1 ? argv[argc - 1] : "";
    const bool export_events = last_arg.rfind(k_output_option, 0u) == 0u;
    const int profile_argc = argc - (export_events ? 1 : 0);
    const std::string output_path = export_events
        ? last_arg.substr(std::char_traits<char>::length(k_output_option))
        : std::string{};

    if ((profile_argc != 4 && profile_argc != 8 && profile_argc != 12) ||
        (export_events && (output_path.empty() || output_path == argv[1])))
    {
        std::fprintf(stderr,
            "usage: ee_issue_trace_candidate <trace.csv> "
            "<integer-result-latency> <load-result-latency> "
            "[<multiply-latency> <divide-latency> <hilo-transfer-latency> "
            "<hilo-accumulate-latency>] "
            "[<fpu-transfer-latency> <fpu-arithmetic-latency> "
            "<fpu-divide-latency> <fpu-accumulate-latency>] "
            "[--candidate-events=<path>]\n"
            "Diagnostic only. Supplied latencies are NOT verified R5900 cycles.\n");
        return 2;
    }

    ee::s_ee_issue_replay_profile profile{};
    if (!parse_latency(argv[2], &profile.m_integer_latency) ||
        !parse_latency(argv[3], &profile.m_load_latency))
    {
        std::fprintf(stderr, "positive, explicit latency values required\n");
        return 2;
    }

    if (profile_argc >= 8 &&
        (!parse_latency(argv[4], &profile.m_hilo_multiply_latency) ||
         !parse_latency(argv[5], &profile.m_hilo_divide_latency) ||
         !parse_latency(argv[6], &profile.m_hilo_transfer_latency) ||
         !parse_latency(argv[7], &profile.m_hilo_accumulate_latency)))
    {
        std::fprintf(stderr, "HI/LO timing requires explicit positive latencies\n");
        return 2;
    }

    if (profile_argc == 12 &&
        (!parse_latency(argv[8], &profile.m_fpu_transfer_latency) ||
         !parse_latency(argv[9], &profile.m_fpu_arithmetic_latency) ||
         !parse_latency(argv[10], &profile.m_fpu_divide_latency) ||
         !parse_latency(argv[11], &profile.m_fpu_accumulate_latency)))
    {
        std::fprintf(stderr, "COP1 timing requires explicit positive latencies\n");
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input)
    {
        std::fprintf(stderr, "cannot open issue trace: %s\n", argv[1]);
        return 2;
    }

    std::string line;
    if (!std::getline(input, line))
    {
        std::fprintf(stderr, "empty issue trace\n");
        return 2;
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    if (line != "attempt,pc,opcode_word,kind,reference_slot,hle_service")
    {
        std::fprintf(stderr, "unsupported issue trace CSV schema\n");
        return 2;
    }

    ee::c_ee_issue_trace_replay replay{};
    if (!replay.configure(profile))
    {
        return 2;
    }

    uint64_t event_count = 0u;
    bool complete_footer = false;
    std::vector<s_candidate_event> candidates;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.rfind("# truncated=", 0u) == 0u)
        {
            complete_footer = line.rfind(
                "# truncated=false range=", 0u) == 0u;
            break;
        }

        ee::s_ee_instruction_trace_entry entry{};
        if (!parse_entry(line, &entry))
        {
            std::fprintf(stderr, "invalid observation at row %llu\n",
                static_cast<unsigned long long>(event_count + 2u));
            return 3;
        }
        const auto result = replay.observe(entry);
        if (result != ee::e_ee_issue_replay_status::ISSUED &&
            result != ee::e_ee_issue_replay_status::ANNULLED)
        {
            std::fprintf(stderr,
                "unmodeled or incomplete R5900 timing: pc=0x%08x "
                "reference_slot=%llu result=%u "
                "(NO hardware timestamp produced)\n",
                replay.failure_pc(),
                static_cast<unsigned long long>(entry.m_reference_slot),
                static_cast<unsigned>(result));
            return 3;
        }
        if (export_events)
        {
            // Native runner captures at most 65536 observations. Leave
            // generous room for external traces without unbounded memory.
            if (candidates.size() >= 1048576u)
            {
                std::fprintf(stderr, "candidate export event cap exceeded\n");
                return 3;
            }
            candidates.push_back({
                entry.m_reference_slot, entry.m_pc, entry.m_opcode_word,
                entry.m_kind, replay.last_event_candidate_cycle()
            });
        }
        ++event_count;
    }
    if (!complete_footer || std::getline(input, line))
    {
        std::fprintf(stderr,
            "trace is truncated or lacks a complete footer "
            "(NO absolute timing claimed)\n");
        return 3;
    }

    if (export_events)
    {
        FILE* output = std::fopen(output_path.c_str(), "wb");
        if (output == nullptr)
        {
            std::fprintf(stderr, "cannot open output: %s\n", output_path.c_str());
            return 3;
        }
        std::fprintf(output,
            "reference_slot,pc,opcode_word,kind,candidate_issue_cycle\n");
        for (const s_candidate_event& item : candidates)
        {
            std::fprintf(output, "%llu,0x%08x,0x%08x,%u,%llu\n",
                static_cast<unsigned long long>(item.m_slot),
                item.m_pc, item.m_opcode, item.m_kind,
                static_cast<unsigned long long>(item.m_cycle));
        }
        const bool written = std::ferror(output) == 0;
        const bool closed = std::fclose(output) == 0;
        if (!written || !closed)
        {
            std::remove(output_path.c_str());
            std::fprintf(stderr, "failed to write complete candidate timeline\n");
            return 3;
        }
    }

    std::printf(
        "candidate_issue_cycles=%llu reached=%llu annulled=%llu "
        "events=%llu profile_integer=%u profile_load=%u "
        "(EXPERIMENTAL; NOT validated R5900 cycles; NO GS events)\n",
        static_cast<unsigned long long>(replay.candidate_cycles()),
        static_cast<unsigned long long>(replay.executed_instructions()),
        static_cast<unsigned long long>(replay.annulled_slots()),
        static_cast<unsigned long long>(event_count),
        profile.m_integer_latency, profile.m_load_latency
    );
    return 0;
}
