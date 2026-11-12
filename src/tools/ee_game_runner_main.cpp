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
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

constexpr uint32_t k_ee_ram_size = 32u * 1024u * 1024u;
constexpr uint32_t k_default_stack = 0x01fff000u;
constexpr uint32_t k_default_dispatch_budget = 10000u;
constexpr uint32_t k_report_history_limit = 64u;

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
        parsed > (std::numeric_limits<uint32_t>::max)())
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
        "  --hle-startup        Handle selected startup and semaphore kernel calls\n"
        "  --kseg-ram           Alias KSEG0/KSEG1 addresses into 32 MiB EE RAM\n"
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

struct s_dispatch_sample
{
    uint64_t m_index = 0;
    uint32_t m_from = 0;
    uint32_t m_to = 0;
    uint32_t m_reason = 0;
    uint32_t m_status = 0;
};

bool write_report(
    const char* path,
    const ee::elf_loader& image,
    const ee::s_ee_cpu_state& cpu,
    uint32_t entry,
    uint64_t dispatched,
    uint32_t reason,
    uint32_t status,
    const std::vector<s_dispatch_sample>& history
)
{
    FILE* output = std::fopen(path, "wb");
    if (output == nullptr)
    {
        std::fprintf(stderr, "failed to create runner report: %s\n", path);
        return false;
    }
    const uint32_t cause = cpu.m_cop0[13];
    const uint32_t epc = cpu.m_cop0[14];
    uint32_t instruction = 0;
    const bool has_instruction = image.read32_at(epc, &instruction);
    std::fprintf(output,
        "{\n  \"schema\": \"ee-game-run-report-v1\","
        "\n  \"entry_pc\": %u, \"final_pc\": %u,"
        "\n  \"dispatches\": %llu, \"exit_reason\": %u,"
        "\n  \"exit_name\": \"%s\", \"dispatch_status\": %u,"
        "\n  \"cop0_cause\": %u, \"cop0_epc\": %u,"
        "\n  \"cop0_status\": %u, \"bad_vaddr\": %u,"
        "\n  \"epc_instruction\": ",
        entry, cpu.m_pc, static_cast<unsigned long long>(dispatched),
        reason, exit_name(reason), status, cause, epc,
        cpu.m_cop0[12], cpu.m_cop0[8]);
    if (has_instruction)
    {
        std::fprintf(output, "%u", instruction);
    }
    else
    {
        std::fputs("null", output);
    }
    std::fputs(",\n  \"gpr_lo\": [", output);
    for (uint32_t i = 0; i < 32; ++i)
    {
        std::fprintf(output, "%s%llu", i ? ", " : "",
            static_cast<unsigned long long>(cpu.m_gpr[i].m_lo));
    }
    std::fputs("],\n  \"history\": [", output);
    for (size_t i = 0; i < history.size(); ++i)
    {
        const auto& item = history[i];
        std::fprintf(output,
            "%s\n    {\"dispatch\": %llu, \"from_pc\": %u,"
            " \"to_pc\": %u, \"reason\": %u, \"status\": %u}",
            i ? "," : "", static_cast<unsigned long long>(item.m_index),
            item.m_from, item.m_to, item.m_reason, item.m_status);
    }
    std::fputs(history.empty() ? "]\n}\n" : "\n  ]\n}\n", output);
    const bool no_write_error = std::ferror(output) == 0;
    const bool closed = std::fclose(output) == 0;
    const bool ok = no_write_error && closed;
    if (!ok)
    {
        std::fprintf(stderr, "failed to write complete runner report: %s\n", path);
    }
    return ok;
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
    bool hle_startup = false;
    bool kseg_ram = false;
    const char* report_path = nullptr;
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
        if (argument.compare(0, 9, "--report=") == 0)
        {
            if (argument.size() == 9)
            {
                std::fprintf(stderr, "empty report path\n");
                return 2;
            }
            report_path = argv[i] + 9;
            continue;
        }
        if (argument == "--kseg-ram")
        {
            kseg_ram = true;
            continue;
        }
        if (argument == "--hle-startup")
        {
            hle_startup = true;
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

    memory->set_kseg_ram_aliases(kseg_ram);

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

    uint32_t startup_stack_top = stack;
    uint32_t startup_stack_size = 0;
    bool startup_thread_set = false;

    struct s_kernel_semaphore
    {
        bool m_used = false;
        uint32_t m_count = 0;
        uint32_t m_max_count = 0;
        uint32_t m_attr = 0;
        uint32_t m_option = 0;
    };
    s_kernel_semaphore semaphores[64]{};
    uint32_t installed_syscall_handlers[256]{};

    uint32_t remaining = budget;
    uint64_t dispatched = 0;
    std::vector<s_dispatch_sample> history;
    if (report_path != nullptr)
    {
        history.reserve(k_report_history_limit);
    }
    uint32_t status = 0;
    uint32_t reason = cpu.m_exit_reason;
    while (remaining != 0)
    {
        const uint32_t from_pc = cpu.m_pc;
        uint32_t count = 0;
        uint32_t out_pc = from_pc;
        const uint32_t slice = (trace || report_path != nullptr) ? 1u : remaining;
        reason = native_run(&cpu, &runtime, slice, &count, &out_pc, &status);

        if (count > slice)
        {
            std::fprintf(stderr, "invalid native dispatcher count %u > %u\n", count, slice);
            return 1;
        }

        dispatched += count;
        remaining -= count;
        if (report_path != nullptr && count != 0)
        {
            if (history.size() == k_report_history_limit)
            {
                history.erase(history.begin());
            }
            history.push_back({dispatched, from_pc, out_pc, reason, status});
        }

        if (trace)
        {
            std::printf("dispatch=%llu pc=0x%08x -> 0x%08x reason=%s(%u) status=%u\n",
                        static_cast<unsigned long long>(dispatched), from_pc,
                        out_pc, exit_name(reason), reason, status);
        }

        // A translated return to PC zero is an exit from the guest code
        // region. Do not invoke another one-dispatch slice with PC zero:
        // the dispatcher reserves zero as an initial-entry sentinel.
        if (count != 0u && cpu.m_pc == 0u)
        {
            status = 0u;
            break;
        }

        // Experimental PS2 EE startup HLE. Handle only explicitly
        // recognized syscalls; unknown services still stop at the exception.
        if (hle_startup &&
            reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) &&
            ((cpu.m_cop0[13] >> 2) & 0x1fu) == 8u)
        {
            const uint32_t service =
                static_cast<uint32_t>(cpu.m_gpr[3].m_lo);
            if (service == 0x3cu || service == 0x3du ||
                service == 0x40u || service == 0x74u)
            {
                const uint32_t epc = cpu.m_cop0[14];
                uint32_t opcode = 0;
                if (!image.read32_at(epc, &opcode) ||
                    (opcode & 0x3fu) != 0x0cu ||
                    (opcode >> 26) != 0u ||
                    (cpu.m_cop0[13] & 0x80000000u) != 0u ||
                    epc > 0xfffffffbu)
                {
                    std::fprintf(stderr, "startup HLE refused invalid syscall context\n");
                    break;
                }

                if (service == 0x3cu)
                {
                    const uint32_t stack_arg =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const uint32_t requested_size =
                        static_cast<uint32_t>(cpu.m_gpr[6].m_lo);
                    uint32_t new_stack = 0;
                    if (requested_size < 0x100u ||
                        requested_size > k_ee_ram_size)
                    {
                        std::fprintf(stderr, "SetupThread HLE refused stack size\n");
                        break;
                    }
                    if (stack_arg == 0xffffffffu)
                    {
                        new_stack = stack;
                    }
                    else
                    {
                        const uint64_t end =
                            static_cast<uint64_t>(stack_arg) + requested_size;
                        if (end > k_ee_ram_size)
                        {
                            std::fprintf(stderr, "SetupThread HLE refused stack region\n");
                            break;
                        }
                        new_stack = static_cast<uint32_t>(end) & ~0x0fu;
                    }
                    if (new_stack < requested_size ||
                        !memory->contains(new_stack - requested_size, requested_size))
                    {
                        std::fprintf(stderr, "SetupThread HLE refused unbacked stack\n");
                        break;
                    }
                    startup_stack_top = new_stack;
                    startup_stack_size = requested_size;
                    startup_thread_set = true;
                    cpu.m_gpr[2].m_lo = new_stack;
                    if (trace)
                    {
                        std::printf(
                            "HLE SetupThread: gp=0x%08x stack=0x%08x "
                            "size=0x%08x args=0x%08x\n",
                            static_cast<uint32_t>(cpu.m_gpr[4].m_lo),
                            new_stack, requested_size,
                            static_cast<uint32_t>(cpu.m_gpr[7].m_lo)
                        );
                    }
                }
                else if (service == 0x3du)
                {
                    const uint32_t request_base =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t request_size =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const uint64_t aligned_base =
                        (static_cast<uint64_t>(request_base) + 15u) & ~15ull;
                    const uint64_t limit = startup_thread_set
                        ? static_cast<uint64_t>(startup_stack_top - startup_stack_size)
                        : k_ee_ram_size;
                    const uint64_t heap_end = request_size == 0xffffffffu
                        ? limit : aligned_base + request_size;

                    if (!startup_thread_set ||
                        aligned_base > limit ||
                        heap_end < aligned_base ||
                        heap_end > limit)
                    {
                        std::fprintf(stderr, "SetupHeap HLE refused overlapping heap/stack\n");
                        break;
                    }
                    // A real kernel records the heap extent in its process
                    // state. We validate the request here, but do not claim
                    // malloc/sbrk or a general EE memory manager exists.
                    if (trace)
                    {
                        std::printf(
                            "HLE SetupHeap: base=0x%08x end=0x%08x "
                            "size=0x%08x\n",
                            static_cast<uint32_t>(aligned_base),
                            static_cast<uint32_t>(heap_end), request_size
                        );
                    }
                }

                else if (service == 0x40u)
                {
                    // PS2SDK ee_sema_t is six consecutive 32-bit fields:
                    // count, max_count, init_count, wait_threads, attr,
                    // option. Initial allocation uses max_count/init_count;
                    // count and wait_threads are runtime output fields.
                    const uint32_t parameter_address =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    if ((parameter_address & 3u) != 0 ||
                        !memory->contains(parameter_address, 24u))
                    {
                        std::fprintf(stderr, "CreateSema HLE refused unbacked parameter block\n");
                        break;
                    }
                    const uint32_t max_count =
                        memory->read32(parameter_address + 4u);
                    const uint32_t init_count =
                        memory->read32(parameter_address + 8u);
                    const uint32_t attr =
                        memory->read32(parameter_address + 16u);
                    const uint32_t option =
                        memory->read32(parameter_address + 20u);
                    if (max_count == 0u || max_count > 0x7fffffffu ||
                        init_count > max_count)
                    {
                        std::fprintf(stderr,
                            "CreateSema HLE refused invalid count init=%u max=%u\n",
                            init_count, max_count
                        );
                        break;
                    }
                    uint32_t id = 0u;
                    for (uint32_t candidate = 1u; candidate <= 64u; ++candidate)
                    {
                        if (!semaphores[candidate - 1u].m_used)
                        {
                            id = candidate;
                            break;
                        }
                    }
                    if (id == 0u)
                    {
                        std::fprintf(stderr, "CreateSema HLE exhausted semaphore slots\n");
                        break;
                    }
                    s_kernel_semaphore& semaphore = semaphores[id - 1u];
                    semaphore.m_used = true;
                    semaphore.m_count = init_count;
                    semaphore.m_max_count = max_count;
                    semaphore.m_attr = attr;
                    semaphore.m_option = option;
                    cpu.m_gpr[2].m_lo = id;
                    if (trace)
                    {
                        std::printf(
                            "HLE CreateSema: id=%u init=%u max=%u attr=0x%08x "
                            "option=0x%08x parameters=0x%08x\n",
                            id, init_count, max_count, attr, option,
                            parameter_address
                        );
                    }
                }

                else
                {
                    // SetSyscall(number, handler) installs an EE kernel
                    // callback. Store it for future dispatch, but do not
                    // silently emulate an invocation of that callback.
                    const uint32_t syscall_number =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t handler_pc =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    if (syscall_number >= 256u ||
                        (handler_pc & 3u) != 0u ||
                        !image.is_executable_address(handler_pc))
                    {
                        std::fprintf(stderr,
                            "SetSyscall HLE refused invalid handler "
                            "number=0x%08x pc=0x%08x\n",
                            syscall_number, handler_pc
                        );
                        break;
                    }
                    installed_syscall_handlers[syscall_number] = handler_pc;
                    if (trace)
                    {
                        std::printf(
                            "HLE SetSyscall: number=0x%02x "
                            "handler=0x%08x\n",
                            syscall_number, handler_pc
                        );
                    }
                }

                cpu.m_cop0[12] &= ~2u; // Clear COP0.Status.EXL.
                cpu.m_pc = epc + 4u;
                cpu.m_exit_reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_DISPATCH
                );
                continue;
            }
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
    if (reason == static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_EXCEPTION))
    {
        // COP0 Cause[6:2] describes the exception; EPC identifies the
        // original guest instruction, not the architectural vector PC.
        const uint32_t cause = cpu.m_cop0[13];
        const uint32_t epc = cpu.m_cop0[14];
        const uint32_t exception_code = (cause >> 2) & 0x1fu;
        uint32_t instruction = 0;
        const bool has_instruction = image.read32_at(epc, &instruction);
        std::printf(
            "EE exception: code=%u EPC=0x%08x Cause=0x%08x "
            "Status=0x%08x BadVAddr=0x%08x\n",
            exception_code, epc, cause,
            cpu.m_cop0[12], cpu.m_cop0[8]
        );
        if (has_instruction)
        {
            std::printf("  instruction=0x%08x", instruction);
            if (exception_code == 8u && (instruction & 0x3fu) == 0x0cu)
            {
                std::printf(" syscall_code=0x%05x", (instruction >> 6) & 0xfffffu);
            }
            std::printf("\n");
        }
        if (exception_code == 8u)
        {
            std::printf(
                "  v0=0x%016llx v1=0x%016llx "
                "a0=0x%016llx a1=0x%016llx "
                "a2=0x%016llx a3=0x%016llx\n",
                static_cast<unsigned long long>(cpu.m_gpr[2].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[3].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[4].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[5].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[6].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[7].m_lo)
            );
        }
    }
    if (report_path != nullptr &&
        !write_report(report_path, image, cpu, entry, dispatched,
                      reason, status, history))
    {
        return 1;
    }
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
