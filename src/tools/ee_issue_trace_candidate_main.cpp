#include "ee_issue_trace_replay.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>

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

bool parse_entry(const std::string& line,
                 ee::s_ee_instruction_trace_entry* entry)
{
    unsigned long long attempt = 0u;
    unsigned long long slot = 0u;
    unsigned int pc = 0u;
    unsigned int opcode = 0u;
    unsigned int kind = 0u;
    int consumed = 0;
    const int parsed = std::sscanf(
        line.c_str(), "%llu,0x%x,0x%x,%u,%llu%n",
        &attempt, &pc, &opcode, &kind, &slot, &consumed
    );
    if (parsed != 5 || consumed != static_cast<int>(line.size()))
    {
        return false;
    }
    entry->m_attempt = static_cast<uint64_t>(attempt);
    entry->m_pc = static_cast<uint32_t>(pc);
    entry->m_opcode_word = static_cast<uint32_t>(opcode);
    entry->m_kind = static_cast<uint32_t>(kind);
    entry->m_reference_slot = static_cast<uint64_t>(slot);
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr,
            "usage: ee_issue_trace_candidate <trace.csv> "
            "<integer-result-latency> <load-result-latency>\n"
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
    if (line != "attempt,pc,opcode_word,kind,reference_slot")
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
        ++event_count;
    }
    if (!complete_footer || std::getline(input, line))
    {
        std::fprintf(stderr,
            "trace is truncated or lacks a complete footer "
            "(NO absolute timing claimed)\n");
        return 3;
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
