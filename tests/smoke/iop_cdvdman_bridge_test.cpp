#include "iop_cdvdman_bridge.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
void put16(std::vector<uint8_t>& bytes, uint32_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

int fail(const char* message)
{
    std::fprintf(stderr, "IOP CDVDMAN HLE: %s\n", message);
    return 1;
}

struct s_temp_directory
{
    std::filesystem::path m_path;
    ~s_temp_directory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
};

bool write_string(iop::c_cpu& cpu, uint32_t address,
                  const std::string& value)
{
    for (size_t i = 0u; i <= value.size(); i += 4u)
    {
        uint32_t word = 0u;
        for (size_t j = 0u; j < 4u && i + j < value.size(); ++j)
        {
            word |= static_cast<uint32_t>(
                static_cast<uint8_t>(value[i + j])
            ) << (j * 8u);
        }
        if (!cpu.write32(address + static_cast<uint32_t>(i), word))
        {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> make_irx(uint16_t ordinal)
{
    // Synthetic R3000A module, calling the actual linked cdvdman import.
    std::vector<uint8_t> image(0x200u, 0u);
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = image[5] = image[6] = 1u;
    put16(image, 16u, 0xff80u);
    put16(image, 18u, 8u);
    put32(image, 28u, 0x34u);
    put16(image, 40u, 52u);
    put16(image, 42u, 32u);
    put16(image, 44u, 2u);

    put32(image, 0x34u, 1u);           // PT_LOAD
    put32(image, 0x38u, 0x100u);
    put32(image, 0x44u, 0x70u);
    put32(image, 0x48u, 0x80u);

    put32(image, 0x54u, 0x70000080u); // PT_SCE_IOPMOD
    put32(image, 0x58u, 0x180u);
    put32(image, 0x184u, 0u);         // entry
    put32(image, 0x188u, 0x60u);      // gp
    put32(image, 0x18cu, 0x40u);
    put32(image, 0x190u, 0x20u);
    put32(image, 0x194u, 0x20u);

    put32(image, 0x100u, 0x3c040003u); // lui a0,3
    put32(image, 0x104u, 0x34841000u); // ori a0,a0,0x1000
    put32(image, 0x108u, 0x3c050003u); // lui a1,3
    put32(image, 0x10cu, 0x34a51020u); // ori a1,a1,0x1020
    put32(image, 0x110u, 0x0c00800du); // jal 0x20034 (linked import)
    put32(image, 0x114u, 0x00000000u); // delay slot nop
    put32(image, 0x118u, 0x08008006u); // j 0x20018 (idle)
    put32(image, 0x11cu, 0x00000000u); // delay slot nop

    put32(image, 0x120u, 0x41e00000u);
    const char library[] = "cdvdman";
    for (uint32_t i = 0u; i < sizeof(library) - 1u; ++i)
    {
        image[0x12cu + i] = static_cast<uint8_t>(library[i]);
    }
    put32(image, 0x134u, 0x03e00008u); // jr ra
    put32(image, 0x138u, 0x24000000u | ordinal);
    return image;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool read_output(iop::c_cpu& cpu, uint32_t address,
                 std::vector<uint8_t>* output)
{
    output->clear();
    for (uint32_t i = 0u; i < 32u; i += 4u)
    {
        uint32_t word = 0u;
        if (!cpu.read32(address + i, &word))
        {
            return false;
        }
        for (uint32_t j = 0u; j < 4u; ++j)
        {
            output->push_back(static_cast<uint8_t>(word >> (j * 8u)));
        }
    }
    return true;
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().
        time_since_epoch().count();
    s_temp_directory temporary{
        fs::temp_directory_path() /
            ("iop-cdvdman-" + std::to_string(stamp))
    };
    std::error_code error;
    fs::create_directories(temporary.m_path / "DATA", error);
    if (error)
    {
        return fail("could not create test directory");
    }
    const std::string payload = "actual-disc-data";
    {
        std::ofstream output(temporary.m_path / "DATA" / "MiXeD.DAT",
                             std::ios::binary);
        output.write(payload.data(),
                     static_cast<std::streamsize>(payload.size()));
        if (!output)
        {
            return fail("could not write test fixture");
        }
    }
    iop::c_folder_sector_source media(temporary.m_path);
    iop::s_folder_file_info expected{};
    if (!media.valid() ||
        !media.find_file("cdrom0:/data/mixed.dat;1", &expected))
    {
        return fail("synthetic folder media not indexed");
    }

    iop::c_kernel kernel;
    const std::vector<uint8_t> irx = make_irx(10u);
    if (kernel.load_module(irx).m_status !=
            iop::e_irx_load_result::unresolved_import)
    {
        return fail("missing cdvdman service was accepted");
    }

    iop::c_cdvdman_bridge bridge(kernel, media);
    if (!bridge.install())
    {
        return fail("failed to install cdvdman #10 import trampoline");
    }
    if (bridge.install() || bridge.handle_syscall(kernel, 0xffffffffu) ||
        kernel.load_module(make_irx(11u)).m_status !=
            iop::e_irx_load_result::unresolved_import)
    {
        return fail("duplicate install or unknown cdvdman export accepted");
    }

    const iop::s_module_load_result module = kernel.load_module(irx);
    if (module.m_status != iop::e_irx_load_result::success)
    {
        return fail("real synthetic cdvdman IRX import did not link");
    }
    const uint32_t id = kernel.create_module_thread(module.m_id, 1u);
    if (id == 0u || !kernel.start_thread(id))
    {
        return fail("synthetic cdvdman module was not scheduled");
    }

    // No media search occurs during link/install. The guest executes
    // its import stub, passing sceCdlFILE* in a0 and path in a1.
    for (uint32_t offset = 0u; offset < 32u; offset += 4u)
    {
        if (!kernel.cpu().write32(0x31000u + offset, 0xaaaaaaaau))
        {
            return fail("could not initialize output sentinel");
        }
    }
    if (!write_string(kernel.cpu(), 0x31020u, "\\data\\mixed.dat;1"))
    {
        return fail("could not initialize guest filename");
    }
    std::vector<uint8_t> before;
    if (!read_output(kernel.cpu(), 0x31000u, &before) ||
        before[0] != 0xaau || bridge.handle_syscall(kernel, 0xffffffffu))
    {
        return fail("install fabricated a search result");
    }

    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return bridge.handle_syscall(active, service);
    });
    const iop::s_kernel_run_result result = kernel.run(64u, 64u);
    const iop::s_kernel_thread* thread = kernel.thread(id);
    std::vector<uint8_t> output;
    if (result.m_stop != iop::e_kernel_stop::budget_exhausted ||
        thread == nullptr || thread->m_cpu.m_gpr[2] != 1u ||
        !read_output(kernel.cpu(), 0x31000u, &output) ||
        read32(output, 0u) != expected.m_lba ||
        read32(output, 4u) != expected.m_size ||
        std::string(reinterpret_cast<const char*>(output.data() + 8u)) !=
            "MIXED.DAT;1" ||
        output[24] != 0u || output[25] != 0u ||
        output[26] != 0u || output[27] != 0u ||
        output[28] != 1u || output[29] != 1u ||
        output[30] != 0xeau || output[31] != 0x07u)
    {
        return fail("guest IRX import did not return accurate sceCdlFILE");
    }

    // The returned LBA must resolve to actual file data through the
    // same sector source used by CDVD DMA3.
    std::vector<uint8_t> bytes;
    if (!media.read_sectors(expected.m_lba, 1u, bytes) ||
        std::string(reinterpret_cast<const char*>(bytes.data()),
                    payload.size()) != payload)
    {
        return fail("returned LBA does not read actual file bytes");
    }

    // Lookup misses are successful HLE dispatches with v0=0 and must
    // leave the full output buffer intact. Invalid pointers fail closed.
    const std::vector<uint8_t> saved = output;
    if (!write_string(kernel.cpu(), 0x31020u, "\\DATA\\ABSENT.DAT;1"))
    {
        return fail("cannot write missing guest filename");
    }
    kernel.cpu().state().m_gpr[4] = 0x31000u;
    kernel.cpu().state().m_gpr[5] = 0x31020u;
    if (!bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        !read_output(kernel.cpu(), 0x31000u, &output) ||
        output != saved)
    {
        return fail("missing file did not return zero and preserve output");
    }

    if (!write_string(kernel.cpu(), 0x31020u, "\\DATA\\MIXED.DAT;2") ||
        !bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        !read_output(kernel.cpu(), 0x31000u, &output) ||
        output != saved)
    {
        return fail("unsupported ISO version returned a false match");
    }

    kernel.cpu().state().m_gpr[4] = 0x001ffff0u;
    if (bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        !read_output(kernel.cpu(), 0x31000u, &output) || output != saved)
    {
        return fail("out-of-range guest output pointer was accepted");
    }
    kernel.cpu().state().m_gpr[4] = 0x31000u;
    kernel.cpu().state().m_gpr[5] = 0u;
    if (bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        !read_output(kernel.cpu(), 0x31000u, &output) || output != saved)
    {
        return fail("null guest pathname pointer was accepted");
    }

    std::printf("IOP cdvdman sceCdSearchFile guest import: PASS\n");
    return 0;
}
