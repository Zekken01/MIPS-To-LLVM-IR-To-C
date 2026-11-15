#include "ee_recomp.h"
#include "module_builder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static void print_usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s <elf.elf> [output_dir] [options]\n"
        "\n"
        "options:\n"
        "  --max-functions=N   Emit at most N functions (0 = unlimited)\n"
        "  --function=0xADDR   Emit only the function at guest address ADDR\n"
        "  --opt-level=N       IR optimisations 0-3 (default 0):\n"
        "                        0/1 safe baseline, 2 +CSE, 3 +dead-temp\n"

        "  --discovery-cache=FILE   Reuse matching full-ELF discovery for --function\n"
        "  -h, --help          Show this help\n",
        argv0);
}

static uint32_t parse_u32(const std::string& s)
{
    return (uint32_t)std::strtoul(s.c_str(), nullptr, 0);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    const char* elf_path = nullptr;
    const char* output_dir = "build/recompiled";
    ee::s_recomp_options opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg.compare(0, 16, "--max-functions=") == 0) {
            opts.max_functions = parse_u32(arg.substr(16));
            continue;
        }
        if (arg.compare(0, 11, "--function=") == 0) {
            opts.only_function = parse_u32(arg.substr(11));
            continue;
        }
        if (arg.compare(0, 18, "--discovery-cache=") == 0) {
            opts.discovery_cache_path = arg.substr(18);
            continue;
        }
        if (arg.compare(0, 12, "--opt-level=") == 0) {
            opts.opt_level = (int)parse_u32(arg.substr(12));
            continue;
        }
        if (arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 2;
        }
        if (!elf_path) {
            elf_path = argv[i];
        } else {
            output_dir = argv[i];
        }
    }

    if (!elf_path) {
        print_usage(argv[0]);
        return 2;
    }

    std::printf("ee_recomp: loading %s -> %s", elf_path, output_dir);
    if (opts.max_functions)
        std::printf(" (max-functions=%u)", opts.max_functions);
    if (opts.only_function)
        std::printf(" (function=0x%08x)", opts.only_function);
    if (opts.opt_level)
        std::printf(" (opt-level=%d)", opts.opt_level);
    std::printf("\n");
    std::fflush(stdout);

    ee::s_recompiled_elf result;
    std::string error;
    if (!ee::ee_recomp(elf_path, output_dir, &result, &error, opts)) {
        std::fprintf(stderr, "ee_recomp: %s\n", error.c_str());
        return 1;
    }
    std::printf("ee_recomp: done\n");
    std::fflush(stdout);

    std::printf("\n== ee_recomp summary ==\n");
    std::printf("elf: %s\n", result.path.c_str());
    std::printf("entry: 0x%08x  machine: %u  segments: %u\n",
        result.entry_point, result.machine, result.segment_count);
    std::printf("functions emitted: %u  discovered: %u\n",
        result.function_count, result.functions_discovered);
    std::printf("function-scoped: pcs=%u  insns=%u  supported=%u  unsupported=%u",
        result.function_scoped_pcs, result.total_insns,
        result.total_supported, result.total_unsupported);
    if (result.total_insns > 0) {
        double pct = 100.0 * result.total_supported / result.total_insns;
        std::printf("  (%.1f%%)", pct);
    }
    std::printf("\n");
    std::printf("linear sweep:    insns=%u  supported=%u  unsupported=%u",
        result.linear_insns, result.linear_supported, result.linear_unsupported);
    if (result.linear_insns > 0) {
        double pct = 100.0 * result.linear_supported / result.linear_insns;
        std::printf("  (%.1f%%)", pct);
    }
    std::printf("\n");
    return 0;
}
