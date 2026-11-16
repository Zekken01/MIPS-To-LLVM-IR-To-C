#include "iop_kernel.h"
#include "iop_hw.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
struct s_trace_sample
{
    uint32_t m_pc = 0u;
    uint32_t m_opcode = 0u;
};

const char* stop_name(iop::e_kernel_stop stop)
{
    switch (stop)
    {
    case iop::e_kernel_stop::budget_exhausted: return "BUDGET_EXHAUSTED";
    case iop::e_kernel_stop::idle: return "IDLE";
    case iop::e_kernel_stop::unhandled_syscall: return "UNHANDLED_SYSCALL";
    case iop::e_kernel_stop::unsupported_instruction: return "UNSUPPORTED_INSTRUCTION";
    case iop::e_kernel_stop::memory_fault: return "MEMORY_FAULT";
    case iop::e_kernel_stop::arithmetic_overflow: return "ARITHMETIC_OVERFLOW";
    case iop::e_kernel_stop::breakpoint: return "BREAKPOINT";
    default: return "UNKNOWN";
    }
}

bool parse_unsigned(
    const std::string& text, uint64_t* output
)
{
    if (output == nullptr || text.empty() ||
        text[0] == '-' || text[0] == '+')
    {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(
        text.c_str(), &end, 0
    );
    if (errno != 0 || end == text.c_str() ||
        end == nullptr || *end != '\0')
    {
        return false;
    }
    *output = static_cast<uint64_t>(parsed);
    return true;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool has_romdir_iopboot(const std::vector<uint8_t>& image)
{
    if (image.size() != iop::c_cpu::k_rom0_size)
    {
        return false;
    }

    // Standard ROMDIR records begin at 0x1000, 16 bytes per entry.
    // This is informative: a malformed ROMDIR does not authorize
    // a synthetic IOP boot or fabricated guest module.
    for (size_t offset = 0x1000u;
         offset + 16u <= 0x1800u; offset += 16u)
    {
        const uint8_t* name = image.data() + offset;
        if (name[0] == 0u)
        {
            break;
        }
        size_t count = 0u;
        while (count < 10u && name[count] != 0u)
        {
            ++count;
        }
        if (count == 7u &&
            std::memcmp(name, "IOPBOOT", 7u) == 0 &&
            read32(image, offset + 12u) != 0u)
        {
            return true;
        }
    }
    return false;
}

void usage(const char* program)
{
    std::printf(
        "usage: %s <ROM0.bin> [options]\n"
        "  --instructions=N    Max guest R3000 instructions (default 200000)\n"
        "  --quantum=N         Instruction batch (default 128)\n"
        "  --trace-tail=N      Keep final N guest instructions (default 16, 0=off)\n"
        "  --report=FILE       Write reproducible JSON fault/CPU-state report\n"
        "  --help              Show this help\n"
        "\n"
        "Accepts a 4 MiB BIOS ROM0, including the MIT-licensed\n"
        "OpenPS2BIOS release image. Does not boot the EE, create\n"
        "SMFLAG readiness, supply missing hardware, or load IRXs\n"
        "without an actual firmware request.\n",
        program
    );
}

bool write_report(
    const char* filename, uint64_t budget,
    uint64_t executed, uint32_t reset_pc,
    const iop::c_kernel& kernel,
    const iop::s_kernel_run_result& stopped,
    bool has_iopboot, const std::deque<s_trace_sample>& tail
)
{
    if (filename == nullptr)
    {
        return true;
    }
    std::ofstream output(filename, std::ios::binary);
    if (!output)
    {
        return false;
    }

    const iop::s_cpu_state& cpu = kernel.cpu().state();
    const iop::s_memory_fault& fault =
        kernel.cpu().last_memory_fault();
    const bool actual_fault =
        stopped.m_stop == iop::e_kernel_stop::memory_fault &&
        fault.m_valid;

    output << "{\n"
           << "  \"format\": \"iop-rom0-diagnostics-v1\",\n"
           << "  \"rom0_bytes\": " << iop::c_cpu::k_rom0_size << ",\n"
           << "  \"romdir_has_iopboot\": "
           << (has_iopboot ? "true" : "false") << ",\n"
           << "  \"reset_pc\": " << reset_pc << ",\n"
           << "  \"instruction_budget\": " << budget << ",\n"
           << "  \"executed\": " << executed << ",\n"
           << "  \"stop\": \"" << stop_name(stopped.m_stop) << "\",\n"
           << "  \"pc\": " << cpu.m_pc << ",\n"
           << "  \"opcode\": " << stopped.m_opcode << ",\n"
           << "  \"cop0_status\": " << cpu.m_cop0_status << ",\n"
           << "  \"cop0_cause\": " << cpu.m_cop0_cause << ",\n"
           << "  \"cop0_epc\": " << cpu.m_cop0_epc << ",\n"
           << "  \"cop0_prid\": " << cpu.m_cop0_prid << ",\n"
           << "  \"memory_fault\": ";

    if (actual_fault)
    {
        output << "{\"address\": " << fault.m_address
               << ", \"width\": " << fault.m_width
               << ", \"write\": "
               << (fault.m_write ? "true" : "false")
               << "},\n";
    }
    else
    {
        output << "null,\n";
    }

    output << "  \"gpr\": [";
    for (size_t index = 0u; index < 32u; ++index)
    {
        output << (index == 0u ? "" : ", ")
               << cpu.m_gpr[index];
    }
    output << "],\n  \"sif_iop_subaddr\": "
           << kernel.hardware().sif_smcom()
           << ",\n  \"sif_iop_smflag\": "
           << kernel.hardware().sif_smflag()
           << ",\n  \"console_output_hex\": \"";
    const char* digits = "0123456789abcdef";
    for (unsigned char byte : kernel.hardware().debug_console())
    {
        output << digits[byte >> 4u] << digits[byte & 0x0fu];
    }
    output << "\",\n  \"console_truncated\": "
           << (kernel.hardware().debug_console_truncated()
               ? "true" : "false")
           << ",\n  \"trace_tail\": [";
    for (size_t index = 0u; index < tail.size(); ++index)
    {
        output << (index == 0u ? "" : ", ")
               << "{\"pc\": " << tail[index].m_pc
               << ", \"opcode\": " << tail[index].m_opcode << "}";
    }
    output << "]\n}\n";
    return output.good();
}
} // namespace

int main(int argc, char** argv)
{
    const char* rom0_path = nullptr;
    const char* report_path = nullptr;
    uint64_t budget = 200000u;
    uint64_t quantum = 128u;
    uint64_t trace_tail = 16u;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        const auto option = [&](const char* prefix, uint64_t* value)
        {
            const size_t prefix_len = std::strlen(prefix);
            if (argument.compare(0u, prefix_len, prefix) != 0)
            {
                return 0;
            }
            return parse_unsigned(argument.substr(prefix_len), value)
                ? 1 : -1;
        };
        int parsed = option("--instructions=", &budget);
        if (parsed == 0)
        {
            parsed = option("--quantum=", &quantum);
        }
        if (parsed == 0)
        {
            parsed = option("--trace-tail=", &trace_tail);
        }
        if (parsed < 0)
        {
            std::fprintf(stderr, "invalid numeric option: %s\n",
                         argument.c_str());
            return 2;
        }
        if (parsed == 1)
        {
            continue;
        }
        if (argument.compare(0u, 9u, "--report=") == 0)
        {
            if (argument.size() == 9u || report_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate report path\n");
                return 2;
            }
            report_path = argv[index] + 9;
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            std::fprintf(stderr, "unknown option: %s\n",
                         argument.c_str());
            return 2;
        }
        if (rom0_path != nullptr)
        {
            std::fprintf(stderr, "unexpected positional argument: %s\n",
                         argument.c_str());
            return 2;
        }
        rom0_path = argv[index];
    }
    if (rom0_path == nullptr || budget == 0u ||
        budget > 100000000u || quantum == 0u ||
        quantum > 65536u || trace_tail > 128u)
    {
        usage(argv[0]);
        return 2;
    }

    std::ifstream input(rom0_path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() !=
        static_cast<std::streampos>(iop::c_cpu::k_rom0_size))
    {
        std::fprintf(
            stderr, "IOP ROM0 image must be exactly 4194304 bytes: %s\n",
            rom0_path
        );
        return 2;
    }
    std::vector<uint8_t> image(iop::c_cpu::k_rom0_size);
    input.seekg(0, std::ios::beg);
    if (!input.read(
        reinterpret_cast<char*>(image.data()),
        static_cast<std::streamsize>(image.size())))
    {
        std::fprintf(stderr, "IOP ROM0 read failed: %s\n",
                     rom0_path);
        return 2;
    }
    const bool has_iopboot = has_romdir_iopboot(image);
    iop::c_kernel kernel;
    if (!kernel.start_rom0_boot(image))
    {
        std::fprintf(stderr, "IOP ROM0 reset refused\n");
        return 2;
    }

    const uint32_t start_pc = kernel.cpu().state().m_pc;
    std::printf(
        "IOP ROM0 reset: pc=0x%08x cp0_status=0x%08x "
        "cp0_prid=0x%08x IOPBOOT=%s\n",
        start_pc,
        kernel.cpu().state().m_cop0_status,
        kernel.cpu().state().m_cop0_prid,
        has_iopboot ? "present" : "not found"
    );

    uint64_t executed = 0u;
    std::deque<s_trace_sample> tail;
    iop::s_kernel_run_result stopped{};
    while (executed < budget)
    {
        const uint32_t slice = static_cast<uint32_t>(
            trace_tail != 0u ? 1u :
            std::min<uint64_t>(quantum, budget - executed)
        );
        const uint32_t pc = kernel.cpu().state().m_pc;
        uint32_t opcode = 0u;
        if (trace_tail != 0u)
        {
            (void)kernel.cpu().read32(pc, &opcode);
        }

        stopped = kernel.run(slice, slice);
        executed += stopped.m_executed;
        if (trace_tail != 0u)
        {
            tail.push_back({pc, opcode});
            if (tail.size() > trace_tail)
            {
                tail.pop_front();
            }
        }

        if (stopped.m_stop != iop::e_kernel_stop::budget_exhausted ||
            stopped.m_executed == 0u)
        {
            break;
        }
    }

    const iop::s_cpu_state& cpu = kernel.cpu().state();
    std::printf(
        "IOP ROM0 stopped: reason=%s executed=%llu "
        "pc=0x%08x opcode=0x%08x\n",
        stop_name(stopped.m_stop),
        static_cast<unsigned long long>(executed),
        cpu.m_pc, stopped.m_opcode
    );

    const iop::s_memory_fault& fault = kernel.cpu().last_memory_fault();
    if (stopped.m_stop == iop::e_kernel_stop::memory_fault &&
        fault.m_valid)
    {
        std::printf(
            "IOP fault: %s guest_address=0x%08x width=%u "
            "pc=0x%08x\n",
            fault.m_write ? "write" : "read",
            fault.m_address, fault.m_width, cpu.m_pc
        );
    }
    const std::string& console = kernel.hardware().debug_console();
    if (!console.empty())
    {
        std::printf(
            "IOP console: %zu bytes (truncated=%s)\n",
            console.size(),
            kernel.hardware().debug_console_truncated() ? "yes" : "no"
        );
        (void)std::fwrite(console.data(), 1u, console.size(), stdout);
        if (console.back() != '\n')
        {
            std::putchar('\n');
        }
    }
    std::printf(
        "IOP SIF guest registers: subaddr=0x%08x smflag=0x%08x (EE not connected)\n",
        kernel.hardware().sif_smcom(),
        kernel.hardware().sif_smflag()
    );
    for (const s_trace_sample& item : tail)
    {
        std::printf("IOP trace: pc=0x%08x opcode=0x%08x\n",
                    item.m_pc, item.m_opcode);
    }

    if (!write_report(
        report_path, budget, executed, start_pc,
        kernel, stopped, has_iopboot, tail))
    {
        std::fprintf(stderr, "IOP ROM0 report write failed: %s\n",
                     report_path == nullptr ? "" : report_path);
        return 2;
    }

    // A stop or an instruction budget is not a successful firmware
    // startup. Neither returns zero as a misleading boot-success code.
    return stopped.m_stop ==
        iop::e_kernel_stop::budget_exhausted ? 4 : 3;
}
