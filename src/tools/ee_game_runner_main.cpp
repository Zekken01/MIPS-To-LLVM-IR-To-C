#include "ee_runtime.h"
#include "ee_memory.h"
#include "elf_loader.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

constexpr uint32_t k_ee_ram_size = 32u * 1024u * 1024u;
constexpr uint32_t k_default_stack = 0x01fff000u;
constexpr uint32_t k_default_dispatch_budget = 10000u;

using t_game_run = uint32_t (*)(
    ee::s_ee_cpu_state*,
    ee::s_ee_runtime*,
    uint32_t,
    uint32_t*,
    uint32_t*,
    uint32_t*
);

struct c_native_module
{
#if defined(_WIN32)
    HMODULE m_handle = nullptr;
#else
    void* m_handle = nullptr;
#endif

    ~c_native_module()
    {
#if defined(_WIN32)
        if (m_handle != nullptr)
        {
            FreeLibrary(m_handle);
        }
#else
        if (m_handle != nullptr)
        {
            dlclose(m_handle);
        }
#endif
    }

    bool open(const char* path)
    {
#if defined(_WIN32)
        m_handle = LoadLibraryA(path);
#else
        m_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
        return m_handle != nullptr;
    }

    t_game_run entry() const
    {
#if defined(_WIN32)
        return reinterpret_cast<t_game_run>(
            GetProcAddress(m_handle, "ee_game_run")
        );
#else
        return reinterpret_cast<t_game_run>(dlsym(m_handle, "ee_game_run"));
#endif
    }

    void print_error() const
    {
#if defined(_WIN32)
        std::fprintf(stderr, "native module load failed (Win32 error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
#else
        const char* message = dlerror();
        std::fprintf(stderr, "native module load failed: %s\n",
                     message != nullptr ? message : "unknown dynamic loader error");
#endif
    }
};

bool parse_u32(const char* value, uint32_t* out)
{
    if (value == nullptr || *value == '\0' || *value == '-')
    {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    *out = static_cast<uint32_t>(parsed);
    return true;
}

void usage(const char* program)
{
    std::printf(
        "usage: %s <game.elf> <linked-native-library> [options]\n"
        "  --max-dispatches=N    Native function dispatch budget (default 10000)\n"
        "  --entry=0xADDR       Override ELF entry PC\n"
        "  --stack=0xADDR       Initial EE stack pointer (default 0x01fff000)\n"
        "  --trace              Print guest PC, reason and dispatch count per step\n"
        "  --help               Show this message\n"
        "\n"
        "Runs already-linked translated functions, not a PS2 BIOS/IOP/GS.\n"
        "The limit counts function dispatches, NOT guest instructions.\n",
        program
    );
}

const char* exit_name(uint32_t reason)
{
    switch (static_cast<ee::e_ee_exit_reason>(reason))
    {
    case ee::e_ee_exit_reason::EE_EXIT_RETURN: return "RETURN";
    case ee::e_ee_exit_reason::EE_EXIT_EXCEPTION: return "EXCEPTION";
    case ee::e_ee_exit_reason::EE_EXIT_EXTERNAL_CALL: return "EXTERNAL_CALL";
    case ee::e_ee_exit_reason::EE_EXIT_DISPATCH: return "DISPATCH";
    case ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED: return "UNSUPPORTED";
    default: return "UNKNOWN";
    }
}

bool load_segments(
    const ee::elf_loader& image,
    ee::ee_memory* memory
)
{
    uint32_t count = 0;
    for (const auto& segment : image.segments())
    {
        if (segment.memsz < segment.filesz ||
            segment.data.size() != segment.filesz ||
            !memory->contains(segment.vaddr, segment.memsz))
        {
            std::fprintf(
                stderr,
                "invalid or out-of-RAM PT_LOAD: vaddr=0x%08x file=%u mem=%u\n",
                segment.vaddr, segment.filesz, segment.memsz
            );
            return false;
        }

        // RAM is zero-initialized, so the difference between file and
        // memory sizes represents an already-zeroed BSS tail.
        for (size_t i = 0; i < segment.data.size(); ++i)
        {
            memory->write8(segment.vaddr + static_cast<uint32_t>(i),
                           segment.data[i]);
        }

        ++count;
        std::printf(
            "  PT_LOAD 0x%08x file=%u memory=%u %c%c%c\n",
            segment.vaddr, segment.filesz, segment.memsz,
            segment.readable ? 'r' : '-',
            segment.writable ? 'w' : '-',
            segment.executable ? 'x' : '-'
        );
    }

    if (count == 0)
    {
        std::fprintf(stderr, "ELF has no loadable segments\n");
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const char* elf_path = nullptr;
    const char* library_path = nullptr;
    bool trace = false;
    uint32_t budget = k_default_dispatch_budget;
    uint32_t stack = k_default_stack;
    uint32_t entry_override = 0;
    bool override_entry = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--trace")
        {
            trace = true;
            continue;
        }

        const auto parse_option = [&](const char* prefix, uint32_t* destination) -> int
        {
            const size_t prefix_length = std::strlen(prefix);
            if (argument.compare(0, prefix_length, prefix) != 0)
            {
                return 0;
            }
            return parse_u32(argument.c_str() + prefix_length, destination) ? 1 : -1;
        };

        int parsed = parse_option("--max-dispatches=", &budget);
        if (parsed == 0)
        {
            parsed = parse_option("--stack=", &stack);
        }
        if (parsed == 0)
        {
            parsed = parse_option("--entry=", &entry_override);
            if (parsed == 1)
            {
                override_entry = true;
            }
        }
        if (parsed == -1)
        {
            std::fprintf(stderr, "invalid numeric option: %s\n", argument.c_str());
            return 2;
        }
        if (parsed == 1)
        {
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            return 2;
        }
        if (elf_path == nullptr)
        {
            elf_path = argv[i];
        }
        else if (library_path == nullptr)
        {
            library_path = argv[i];
        }
        else
        {
            std::fprintf(stderr, "unexpected positional argument: %s\n", argument.c_str());
            return 2;
        }
    }

    if (elf_path == nullptr || library_path == nullptr || budget == 0 ||
        stack >= k_ee_ram_size)
    {
        usage(argv[0]);
        return 2;
    }

    ee::elf_loader image;
    std::string error;
    if (!image.load_file(elf_path, &error))
    {
        std::fprintf(stderr, "ELF load failed: %s\n", error.c_str());
        return 1;
    }
    // EM_MIPS is the machine identity of PS2 EE executables.
    if (image.machine() != 8u)
    {
        std::fprintf(stderr, "unsupported ELF machine %u (expected MIPS 8)\n",
                     image.machine());
        return 1;
    }

    const uint32_t entry = override_entry ? entry_override : image.entry_point();
    if (!image.is_executable_address(entry))
    {
        std::fprintf(stderr, "entry 0x%08x is not in an executable PT_LOAD segment\n", entry);
        return 1;
    }

    std::unique_ptr<ee::ee_memory> memory(new ee::ee_memory(k_ee_ram_size));
    if (!memory->contains(0, k_ee_ram_size))
    {
        std::fprintf(stderr, "failed to allocate EE RAM\n");
        return 1;
    }

    std::printf("EE ELF entry 0x%08x; RAM %u MiB; stack 0x%08x\n",
                entry, k_ee_ram_size >> 20, stack);
    if (!load_segments(image, memory.get()))
    {
        return 1;
    }

    c_native_module module;
    if (!module.open(library_path))
    {
        module.print_error();
        return 1;
    }
    const t_game_run native_run = module.entry();
    if (native_run == nullptr)
    {
        std::fprintf(stderr, "native library has no ee_game_run export\n");
        return 1;
    }

    // PS2 hardware devices, syscalls, BIOS and interrupt scheduling
    // are not yet present. This host passes RAM to generated code and
    // reports exits without fabricating a successful guest boot.
    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::s_ee_runtime runtime{};
    runtime.m_user = memory.get();
    runtime.m_vu0_state = &vu0;
    cpu.m_pc = entry;
    cpu.m_gpr[29].m_lo = stack;
    cpu.m_exit_reason = static_cast<uint32_t>(
        ee::e_ee_exit_reason::EE_EXIT_DISPATCH
    );

    uint32_t remaining = budget;
    uint64_t dispatched = 0;
    uint32_t status = 0;
    uint32_t reason = cpu.m_exit_reason;
    while (remaining != 0)
    {
        const uint32_t from_pc = cpu.m_pc;
        uint32_t count = 0;
        uint32_t out_pc = from_pc;
        const uint32_t slice = trace ? 1u : remaining;
        reason = native_run(&cpu, &runtime, slice, &count, &out_pc, &status);

        if (count > slice)
        {
            std::fprintf(stderr, "invalid native dispatcher count %u > %u\n", count, slice);
            return 1;
        }

        dispatched += count;
        remaining -= count;

        if (trace)
        {
            std::printf("dispatch=%llu pc=0x%08x -> 0x%08x reason=%s(%u) status=%u\n",
                        static_cast<unsigned long long>(dispatched), from_pc,
                        out_pc, exit_name(reason), reason, status);
        }

        if (status != 1u || count == 0u)
        {
            break;
        }
    }

    std::printf(
        "EE stopped: dispatched=%llu pc=0x%08x reason=%s(%u) status=%u\n",
        static_cast<unsigned long long>(dispatched), cpu.m_pc,
        exit_name(reason), reason, status
    );
    if (status == 1u && remaining == 0)
    {
        std::fprintf(stderr, "dispatch budget exhausted; resume or raise the limit\n");
        return 4;
    }
    if (status == 2u)
    {
        std::fprintf(stderr, "invalid native dispatch metadata/entry table\n");
        return 3;
    }
    if (dispatched == 0)
    {
        std::fprintf(stderr, "ELF entry has no translated host block; emit the entry function\n");
        return 3;
    }
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED) ||
        reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) ||
        reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXTERNAL_CALL))
    {
        std::fprintf(stderr, "guest blocked by an unsupported operation or host service\n");
        return 3;
    }
    // An exit from the linked translated-code set is not a completed PS2 boot.
    std::fprintf(stderr, "guest left compiled code; further runtime/BIOS work required\n");
    return 3;
}
