#include "iop_disc_loader.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP disc loader: %s\n", message);
    return 1;
}

void put16(std::vector<uint8_t>& bytes, uint32_t at, uint16_t value)
{
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        bytes[at + index] = static_cast<uint8_t>(
            value >> (index * 8u)
        );
    }
}

std::vector<uint8_t> make_irx(bool dependency)
{
    std::vector<uint8_t> bytes(0x220u, 0u);
    bytes[0u] = 0x7fu;
    bytes[1u] = 'E';
    bytes[2u] = 'L';
    bytes[3u] = 'F';
    bytes[4u] = 1u;
    bytes[5u] = 1u;
    bytes[6u] = 1u;
    put16(bytes, 16u, 0xff80u);
    put16(bytes, 18u, 8u);
    put32(bytes, 28u, 0x34u);
    put16(bytes, 40u, 52u);
    put16(bytes, 42u, 32u);
    put16(bytes, 44u, 2u);
    put32(bytes, 0x34u, 1u);
    put32(bytes, 0x38u, 0x100u);
    put32(bytes, 0x44u, 0x60u);
    put32(bytes, 0x48u, 0x80u);
    put32(bytes, 0x54u, 0x70000080u);
    put32(bytes, 0x58u, 0x180u);
    put32(bytes, 0x184u, 0u);
    put32(bytes, 0x188u, 0x50u);
    put32(bytes, 0x18cu, 0x40u);
    put32(bytes, 0x190u, 0x20u);
    put32(bytes, 0x194u, 0x20u);
    const char name[] = "DISCTEST";
    for (uint32_t index = 0u; index < sizeof(name); ++index)
    {
        bytes[0x19au + index] = static_cast<uint8_t>(name[index]);
    }
    put32(bytes, 0x100u, 0x0000000du); // entrypoint: break
    if (dependency)
    {
        put32(bytes, 0x120u, 0x41e00000u);
        const char library[] = "LIBRARY";
        for (uint32_t index = 0u; index < 7u; ++index)
        {
            bytes[0x12cu + index] =
                static_cast<uint8_t>(library[index]);
        }
        put32(bytes, 0x134u, 0x03e00008u);
        put32(bytes, 0x138u, 0x24000007u);
    }
    return bytes;
}

bool write_binary(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes
)
{
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    return output.good();
}

struct s_temp_tree
{
    std::filesystem::path m_path;
    ~s_temp_tree()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
};
} // namespace

int main()
{
    const auto token = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    s_temp_tree folder{
        std::filesystem::temp_directory_path() /
            ("iop-disc-loader-" + std::to_string(token))
    };
    std::filesystem::create_directories(
        folder.m_path / "PS2_GAME" / "SUB"
    );
    const std::filesystem::path disc = folder.m_path / "PS2_GAME";

    if (!write_binary(disc / "PADMAN.IRX", make_irx(true)) ||
        !write_binary(disc / "SUB" / "DEVICE.IRX;1", make_irx(false)))
    {
        return fail("could not write fixture IRX files");
    }

    iop::c_kernel kernel;
    iop::c_disc_loader loader(kernel, disc);

    const auto invalid = [&](const std::string& name,
                              iop::e_disc_load_result status)
    {
        const iop::s_disc_load_result result =
            loader.load_requested(name, true);
        return result.m_result == status &&
               result.m_module_id == 0u &&
               result.m_thread_id == 0u;
    };

    if (!invalid("rom0:PADMAN.IRX", iop::e_disc_load_result::unsupported_device) ||
        !invalid("cdrom0:/../PADMAN.IRX", iop::e_disc_load_result::invalid_guest_path) ||
        !invalid("cdrom0:/SUB//DEVICE.IRX", iop::e_disc_load_result::invalid_guest_path) ||
        !invalid("cdrom0:/SUB/../../DEVICE.IRX", iop::e_disc_load_result::invalid_guest_path) ||
        !invalid("cdrom0:C:/SYSTEM.IRX", iop::e_disc_load_result::invalid_guest_path) ||
        !invalid("cdrom0:/MISSING.IRX;1", iop::e_disc_load_result::missing_file))
    {
        return fail("invalid guest device/path was accepted");
    }

    const iop::s_disc_load_result unresolved =
        loader.load_requested("CDROM0:\\padman.irx;1", true);
    if (unresolved.m_result != iop::e_disc_load_result::link_failed ||
        unresolved.m_link_status !=
            iop::e_irx_load_result::unresolved_import ||
        unresolved.m_module_id != 0u)
    {
        return fail("missing IRX provider must fail before guest execution");
    }

    if (!kernel.register_library("LIBRARY", 7u, 0x00018000u))
    {
        return fail("could not provide required test import");
    }

    const iop::s_disc_load_result linked =
        loader.load_requested("cdrom0:\\PADMAN.IRX;1", true);
    const iop::s_kernel_thread* thread =
        kernel.thread(linked.m_thread_id);
    if (linked.m_result != iop::e_disc_load_result::success ||
        linked.m_module_id != 1u ||
        linked.m_thread_id == 0u ||
        thread == nullptr ||
        thread->m_status != iop::e_thread_status::ready ||
        thread->m_priority != iop::c_kernel::k_default_module_priority ||
        thread->m_module_id != linked.m_module_id ||
        !thread->m_module_entry ||
        thread->m_cpu.m_gpr[31] == 0u ||
        thread->m_cpu.m_pc != 0x20000u)
    {
        return fail("guest IRX could not be linked and scheduled");
    }

    const iop::s_kernel_run_result ran = kernel.run(2u);
    if (ran.m_stop != iop::e_kernel_stop::breakpoint ||
        ran.m_thread_id != linked.m_thread_id ||
        ran.m_guest_pc != 0x20000u)
    {
        return fail("resolved guest IRX entrypoint was not executed");
    }

    // A dependency-only request links the provider, but leaves it
    // dormant until explicit guest module-start.
    const iop::s_disc_load_result provider =
        loader.load_requested("cdrom0:/sub/device.irx;1", false);
    if (provider.m_result != iop::e_disc_load_result::success ||
        provider.m_module_id != 2u ||
        provider.m_thread_id != 0u ||
        kernel.module(provider.m_module_id) == nullptr)
    {
        return fail("unstarted provider resolution failed");
    }


    // A real LOADFILE method-0 request supplies NUL-delimited IRX
    // startup arguments. The IOP _start ABI requires argc and argv
    // pointers into the thread's own guest stack, including argv[0]
    // as the module pathname. This must not point into host memory.
    if (!write_binary(disc / "ARGS.IRX", make_irx(false)))
    {
        return fail("cannot author parameterized IOP module");
    }
    const std::string path = "cdrom0:\\ARGS.IRX;1";
    const std::vector<std::string> args = {"-port", "mc0:"};
    const iop::s_disc_load_result parameterized =
        loader.load_requested(path, true, 1u, args);
    const iop::s_kernel_thread* params_thread =
        kernel.thread(parameterized.m_thread_id);
    if (parameterized.m_result != iop::e_disc_load_result::success ||
        parameterized.m_module_id != 3u ||
        params_thread == nullptr ||
        params_thread->m_priority != 1u ||
        params_thread->m_cpu.m_gpr[31] == 0u ||
        params_thread->m_cpu.m_gpr[4] != 3u ||
        params_thread->m_cpu.m_gpr[5] == 0u ||
        params_thread->m_cpu.m_gpr[29] <
            params_thread->m_stack_base)
    {
        return fail("IOP _start did not receive guest argc/argv");
    }
    const uint32_t argv = params_thread->m_cpu.m_gpr[5];
    uint32_t terminator = 0xffffffffu;
    if (argv < params_thread->m_stack_base ||
        argv >= params_thread->m_stack_base +
                params_thread->m_stack_size ||
        params_thread->m_cpu.m_gpr[29] >= argv ||
        !kernel.cpu().read32(argv + 12u, &terminator) ||
        terminator != 0u)
    {
        return fail("IOP argv pointers escaped the module stack");
    }
    const std::vector<std::string> expected = {
        path, "-port", "mc0:"
    };
    for (uint32_t index = 0u; index < expected.size(); ++index)
    {
        uint32_t pointer = 0u;
        if (!kernel.cpu().read32(argv + index * 4u, &pointer) ||
            pointer < params_thread->m_stack_base ||
            pointer + expected[index].size() >=
                params_thread->m_stack_base +
                    params_thread->m_stack_size)
        {
            return fail("IOP argv element pointer invalid");
        }
        for (uint32_t byte = 0u;
             byte <= expected[index].size(); ++byte)
        {
            uint8_t guest = 0xffu;
            const uint8_t match =
                byte == expected[index].size() ? 0u :
                    static_cast<uint8_t>(expected[index][byte]);
            if (!kernel.cpu().read8(pointer + byte, &guest) ||
                guest != match)
            {
                return fail("IOP argv bytes changed during startup");
            }
        }
    }

    if (!write_binary(disc / "padman.irx;1", make_irx(false)) ||
        !invalid("cdrom0:/PADMAN.IRX;1",
                 iop::e_disc_load_result::ambiguous_file))
    {
        return fail("ambiguous ISO version and spelling not rejected");
    }
    std::filesystem::remove(disc / "padman.irx;1");

    if (!write_binary(disc / "INVALID.IRX", {0x7fu, 'E', 'L', 'F', 0u}) ||
        !invalid("cdrom0:/INVALID.IRX",
                 iop::e_disc_load_result::invalid_file))
    {
        return fail("invalid IRX bytes accepted");
    }

    std::error_code error;
    std::filesystem::create_symlink(
        folder.m_path / "outside.irx", disc / "OUTSIDE.IRX", error
    );
    if (!error &&
        !invalid("cdrom0:/OUTSIDE.IRX",
                 iop::e_disc_load_result::invalid_guest_path))
    {
        return fail("symlink path traversal was accepted");
    }

    std::printf("IOP disc loader: PASS\n");
    return 0;
}
