#include "iop_kernel_hle_bridge.h"
#include "iop_folder_sector_source.h"
#include "iop_sysclib_strings.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "IOP kernel HLE imports: %s\n", reason);
    return 1;
}

void write16(std::vector<uint8_t>& data, size_t at, uint16_t value)
{
    data[at] = static_cast<uint8_t>(value);
    data[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void write32(std::vector<uint8_t>& data, size_t at, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[at + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

struct s_imports
{
    const char* m_name;
    std::vector<uint16_t> m_ordinals;
};

const std::vector<s_imports>& retail_dependencies()
{
    static const std::vector<s_imports> imports = {
        {"loadcore", {6u}},
        {"intrman", {4u, 5u, 6u, 7u, 17u, 18u}},
        {"stdio", {4u}},
        {"dmacman", {28u, 32u, 33u, 34u, 35u}},
        {"thbase", {4u, 6u, 20u}},
        {"thevent", {4u, 6u, 7u, 8u, 10u}},
        {"thsemap", {4u, 6u, 8u, 11u}},
        {"sysclib", {8u, 29u, 36u}}
    };
    return imports;
}

std::vector<uint8_t> synthetic_sio2man_dependencies()
{
    // Structurally equivalent to the original module's eight import
    // libraries and all 28 ordinals. The code and data are synthetic;
    // no game assets or copyrighted IOP instructions are embedded.
    std::vector<uint8_t> image(0x660u, 0u);
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 1u;
    image[5] = 1u;
    image[6] = 1u;
    write16(image, 16u, 0xff80u);
    write16(image, 18u, 8u);
    write32(image, 28u, 0x34u);
    write16(image, 40u, 52u);
    write16(image, 42u, 32u);
    write16(image, 44u, 2u);

    write32(image, 0x34u, 1u); // PT_LOAD
    write32(image, 0x38u, 0x100u);
    write32(image, 0x44u, 0x360u);
    write32(image, 0x48u, 0x380u);
    write32(image, 0x54u, 0x70000080u); // PT_SCE_IOPMOD
    write32(image, 0x58u, 0x600u);
    write32(image, 0x604u, 0u);        // entry
    write32(image, 0x608u, 0x8fb0u);   // out-of-image real SIO2MAN GP
    write32(image, 0x60cu, 0x360u);
    write32(image, 0x610u, 0x10u);
    write32(image, 0x614u, 0x10u);
    write32(image, 0x100u, 0x0000000du); // guest break

    uint32_t at = 0x40u;
    for (const s_imports& group : retail_dependencies())
    {
        write32(image, 0x100u + at, 0x41e00000u);
        for (uint32_t i = 0u; group.m_name[i] != 0; ++i)
        {
            image[0x100u + at + 12u + i] =
                static_cast<uint8_t>(group.m_name[i]);
        }
        at += 20u;
        for (uint16_t ordinal : group.m_ordinals)
        {
            write32(image, 0x100u + at, 0x03e00008u);
            write32(image, 0x100u + at + 4u,
                    0x24000000u | ordinal);
            at += 8u;
        }
    }
    return image;
}

bool call_guest(
    iop::c_kernel& kernel, const std::string& library,
    uint16_t ordinal, const std::array<uint32_t, 4>& arguments,
    uint32_t* result
)
{
    static uint32_t next_program = 0x4000u;
    uint32_t entry = 0u;
    if (!kernel.lookup_library(library, ordinal, &entry) ||
        result == nullptr)
    {
        return false;
    }
    const uint32_t program = next_program;
    next_program += 0x80u;
    // Guest MIPS I: four 32-bit arguments, direct JAL to the HLE
    // import target, and a breakpoint after returning from the stub.
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!kernel.cpu().write32(
                program + i * 8u,
                0x3c040000u | (i << 16u) |
                    (arguments[i] >> 16u)) ||
            !kernel.cpu().write32(
                program + i * 8u + 4u,
                0x34840000u | (i << 21u) | (i << 16u) |
                    (arguments[i] & 0xffffu)))
        {
            return false;
        }
    }
    if (!kernel.cpu().write32(
            program + 32u,
            0x0c000000u | ((entry >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(program + 36u, 0u) ||
        !kernel.cpu().write32(program + 40u, 0x0000000du))
    {
        return false;
    }
    const uint32_t thread_id =
        kernel.create_thread(program, 0u, 1u, 0x400u);
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return false;
    }
    const iop::s_kernel_run_result ran = kernel.run(128u);
    const iop::s_kernel_thread* thread =
        kernel.thread(thread_id);
    if (ran.m_stop != iop::e_kernel_stop::breakpoint ||
        ran.m_guest_pc != program + 40u || thread == nullptr)
    {
        return false;
    }
    *result = thread->m_cpu.m_gpr[2];
    return true;
}
} // namespace

int main()
{
    iop::c_kernel kernel;
    const auto synthetic = synthetic_sio2man_dependencies();

    // Before installation the real eight-library import set must fail
    // on the genuine first unresolved provider and remain atomic.
    const iop::s_module_load_result missing =
        kernel.load_module(synthetic);
    if (missing.m_status != iop::e_irx_load_result::unresolved_import ||
        missing.m_link_diagnostic.m_unresolved_library != "loadcore" ||
        missing.m_link_diagnostic.m_unresolved_ordinal != 6u ||
        missing.m_id != 0u)
    {
        return fail("original SIO2MAN imports did not fail closed");
    }

    iop::c_kernel_hle_bridge hle(kernel);
    if (!hle.install())
    {
        return fail("could not install kernel service-family stubs");
    }
    if (kernel.has_guest_work() ||
        hle.handle_syscall(kernel, 0xffffffffu))
    {
        return fail("merely installing HLE manufactured guest execution");
    }
    for (const s_imports& group : retail_dependencies())
    {
        for (uint16_t ordinal : group.m_ordinals)
        {
            uint32_t entry = 0u;
            uint32_t instruction = 0u;
            if (!kernel.lookup_library(
                    group.m_name, ordinal, &entry) ||
                entry < iop::c_kernel_hle_bridge::k_stub_base ||
                !kernel.cpu().read32(entry + 8u, &instruction) ||
                instruction != 0x0000000cu)
            {
                return fail("original SIO2MAN import missing guest stub");
            }
        }
    }
    uint32_t missing_export = 0u;
    if (kernel.lookup_library("dmacman", 16u, &missing_export))
    {
        return fail("unimplemented HLE import was falsely registered");
    }

    const iop::s_module_load_result linked =
        kernel.load_module(synthetic);
    if (linked.m_status != iop::e_irx_load_result::success ||
        linked.m_id != 1u ||
        kernel.module(1u) == nullptr ||
        kernel.module(1u)->m_irx.m_gp !=
            kernel.module(1u)->m_irx.m_base + 0x8fb0u)
    {
        return fail("complete original SIO2MAN dependency set did not link");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return hle.handle_syscall(active, service);
        }
    );

    uint32_t result = 0u;
    if (!call_guest(
            kernel, "thbase", 20u, {0u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("real guest R3000 GetThreadId import did not run");
    }


    // MODLOAD introspection operates on the actual linked IRX registry.
    // Probe the six implemented ordinals through guest R3000 stubs.
    uint32_t modload_pc = 0u;
    for (uint16_t ordinal : {10u, 13u, 16u, 17u, 18u, 22u})
    {
        if (!kernel.lookup_library("modload", ordinal, &modload_pc))
        {
            return fail("implemented MODLOAD import was not registered");
        }
    }
    for (uint16_t ordinal : {7u, 8u})
    {
        if (kernel.lookup_library("modload", ordinal, &modload_pc))
        {
            return fail("unsupported MODLOAD start/wait import registered");
        }
    }
    kernel.cpu().write32(0x2b00u, 0x0000000du);
    if (!call_guest(
            kernel, "modload", 13u, {0x2b00u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "modload", 13u, {3u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-201))
    {
        return fail("MODLOAD callback registration validation failed");
    }
    if (!call_guest(
            kernel, "modload", 16u,
            {0x2b10u, 4u, 0x2b20u, 0u}, &result) ||
        result != 0u)
    {
        return fail("MODLOAD module ID list call failed");
    }
    uint32_t module_id = 0u;
    uint32_t module_count = 0u;
    if (!kernel.cpu().read32(0x2b10u, &module_id) ||
        !kernel.cpu().read32(0x2b20u, &module_count) ||
        module_id != 1u || module_count != 1u ||
        !call_guest(
            kernel, "modload", 17u,
            {module_id, 0x2b40u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("MODLOAD module list and status failed");
    }
    uint32_t status_id = 0u;
    uint32_t status_entry = 0u;
    if (!kernel.cpu().read32(0x2b40u + 0x38u, &status_id) ||
        !kernel.cpu().read32(0x2b40u + 0x40u, &status_entry) ||
        status_id != module_id ||
        status_entry != kernel.module(module_id)->m_irx.m_entry)
    {
        return fail("MODLOAD status does not reflect the IRX");
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        const char name[] = "irx";
        kernel.cpu().write8(0x2ba0u + i,
                            static_cast<uint8_t>(name[i]));
    }
    if (!call_guest(
            kernel, "modload", 22u,
            {0x2ba0u, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !call_guest(
            kernel, "modload", 18u,
            {0x2ba0u, 0x2bb0u, 4u, 0x2bc0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2bb0u, &module_id) ||
        !kernel.cpu().read32(0x2bc0u, &module_count) ||
        module_id != 1u || module_count != 1u)
    {
        return fail("MODLOAD case-insensitive name search failed");
    }
    if (!call_guest(
            kernel, "modload", 17u,
            {0xffffffffu, 0x2b40u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-202))
    {
        return fail("MODLOAD unknown module did not fail closed");
    }


    // Link a second synthetic IRX from IOP RAM. LoadModuleBuffer must
    // not execute an entrypoint, and the registry must reflect the load.
    const uint32_t guest_image = 0x6000u;
    for (uint32_t i = 0u; i < synthetic.size(); ++i)
    {
        if (!kernel.cpu().write8(guest_image + i, synthetic[i]))
        {
            return fail("could not stage guest IRX buffer");
        }
    }
    if (!call_guest(
            kernel, "modload", 10u,
            {guest_image, 0u, 0u, 0u}, &result) ||
        result != 2u || kernel.module(2u) == nullptr ||
        !call_guest(
            kernel, "modload", 16u,
            {0x2b10u, 1u, 0x2b20u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2b20u, &module_count) ||
        module_count != 2u)
    {
        return fail("MODLOAD buffer linking/count/truncation failed");
    }


    // Actual, read-only IOMAN guest file operations use the same
    // ISO9660 folder sector source as native CDVD HLE.
    namespace fs = std::filesystem;
    const fs::path media = fs::temp_directory_path() /
        "sdbz_ioman_hle_regression";
    std::error_code media_error;
    fs::remove_all(media, media_error);
    media_error.clear();
    fs::create_directories(media, media_error);
    if (media_error)
    {
        return fail("could not create IOMAN media fixture");
    }
    {
        std::ofstream file(media / "HELLO.BIN", std::ios::binary);
        file << "SDBZ DATA";
        if (!file)
        {
            return fail("could not write IOMAN media fixture");
        }
    }
    iop::c_folder_sector_source sector_source(media);
    if (!sector_source.valid())
    {
        return fail("could not mount IOMAN media fixture");
    }
    hle.attach_disc_source(&sector_source);
    const char ioman_path[] = "cdrom0:\\HELLO.BIN;1";
    for (uint32_t i = 0u; i < sizeof(ioman_path); ++i)
    {
        if (!kernel.cpu().write8(
                0x3000u + i, static_cast<uint8_t>(ioman_path[i])))
        {
            return fail("could not install IOMAN path");
        }
    }
    for (uint16_t ordinal : {4u, 5u, 6u, 7u, 8u, 16u, 20u, 21u})
    {
        uint32_t entry = 0u;
        if (!kernel.lookup_library("ioman", ordinal, &entry))
        {
            return fail("IOMAN supported/safe boundary was not installed");
        }
    }
    for (uint16_t ordinal : {4u, 5u, 6u})
    {
        uint32_t entry = 0u;
        if (!kernel.lookup_library("secrman", ordinal, &entry))
        {
            return fail("SECRMAN explicit failed boundary not installed");
        }
    }
    if (!call_guest(
            kernel, "ioman", 16u, {0x3000u, 0x3100u, 0u, 0u},
            &result) || result != 0u)
    {
        return fail("IOMAN getstat failed");
    }
    uint32_t file_mode = 0u;
    uint32_t file_size = 0u;
    if (!kernel.cpu().read32(0x3100u, &file_mode) ||
        !kernel.cpu().read32(0x3108u, &file_size) ||
        file_mode != 0x17u || file_size != 9u)
    {
        return fail("IOMAN getstat wrong native ISO metadata");
    }
    if (!call_guest(
            kernel, "ioman", 4u, {0x3000u, 1u, 0u, 0u},
            &result) || result != 3u)
    {
        return fail("IOMAN open failed");
    }
    if (!call_guest(
            kernel, "ioman", 6u, {3u, 0x3140u, 4u, 0u}, &result) ||
        result != 4u ||
        !call_guest(
            kernel, "ioman", 8u, {3u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "ioman", 6u, {3u, 0x3148u, 9u, 0u}, &result) ||
        result != 9u)
    {
        return fail("IOMAN read/seek failed");
    }
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        uint8_t byte = 0u;
        const char expected[] = "SDBZ DATA";
        if (!kernel.cpu().read8(0x3148u + i, &byte) ||
            byte != static_cast<uint8_t>(expected[i]))
        {
            return fail("IOMAN disc bytes mismatch");
        }
    }
    if (!call_guest(
            kernel, "ioman", 7u, {3u, 0x3140u, 4u, 0u}, &result) ||
        result != static_cast<uint32_t>(-13) ||
        !call_guest(
            kernel, "ioman", 5u, {3u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "ioman", 6u, {3u, 0x3140u, 4u, 0u}, &result) ||
        result != static_cast<uint32_t>(-9))
    {
        return fail("IOMAN failed close/readonly/EBADF semantics");
    }
    if (!call_guest(
            kernel, "ioman", 20u, {0x3100u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "ioman", 21u, {0x3000u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "secrman", 4u, {0u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "secrman", 5u, {0u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "secrman", 6u, {0u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("IOMAN driver or SECRMAN authentication falsely succeeded");
    }
    for (uint16_t ordinal : {9u, 13u, 15u})
    {
        uint32_t entry = 0u;
        if (kernel.lookup_library("ioman", ordinal, &entry))
        {
            return fail("unimplemented IOMAN operation was registered");
        }
    }
    hle.attach_disc_source(nullptr);
    fs::remove_all(media, media_error);

    // The next game loader IRX (MCMAN) requires threadman timing,
    // not merely the original SIO2MAN thread service imports.
    for (uint16_t ordinal : {33u, 34u, 35u, 37u, 39u, 40u})
    {
        uint32_t entry = 0u;
        if (!kernel.lookup_library("thbase", ordinal, &entry) ||
            entry < iop::c_kernel_hle_bridge::k_stub_base)
        {
            return fail("MCMAN timer dependency was not registered");
        }
    }
    for (uint16_t ordinal : {36u, 38u})
    {
        uint32_t entry = 0u;
        if (kernel.lookup_library("thbase", ordinal, &entry))
        {
            return fail("unsupported interrupt-context timer was registered");
        }
    }

    // Exact PS2SDK clock conversion uses 4608/125 ticks per usec.
    // The virtual current clock and wake scheduling are deliberately
    // not claimed to be calibrated to real IOP hardware cycles.
    kernel.cpu().write32(0x2a00u, 0u);
    kernel.cpu().write32(0x2a04u, 0u);
    if (!call_guest(
            kernel, "thbase", 39u, {125u, 0x2a00u, 0u, 0u},
            &result))
    {
        return fail("USec2SysClock guest import did not execute");
    }
    uint32_t low = 0u;
    uint32_t high = 0u;
    uint32_t sec = 0u;
    uint32_t usec = 0u;
    if (!kernel.cpu().read32(0x2a00u, &low) ||
        !kernel.cpu().read32(0x2a04u, &high) ||
        low != 4608u || high != 0u ||
        !call_guest(
            kernel, "thbase", 40u,
            {0x2a00u, 0x2a08u, 0x2a0cu, 0u}, &result) ||
        !kernel.cpu().read32(0x2a08u, &sec) ||
        !kernel.cpu().read32(0x2a0cu, &usec) ||
        sec != 0u || usec != 125u ||
        !call_guest(
            kernel, "thbase", 34u,
            {0x2a10u, 0u, 0u, 0u}, &result) ||
        !kernel.cpu().read32(0x2a10u, &low) ||
        !kernel.cpu().read32(0x2a14u, &high) ||
        (low == 0u && high == 0u) ||
        !call_guest(
            kernel, "thbase", 33u, {1u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("THBASE clock conversion, time or delay failed");
    }

    // A timer's effect must come from executable guest R3000 callback
    // instructions, not an artificial ready bit or ignored completion.
    const uint32_t callback_words[] = {
        0x3c080000u, // lui t0, 0
        0x35082a28u, // ori t0, t0, 0x2a28
        0x24090001u, // addiu t1, zero, 1
        0xad090000u, // sw t1, 0(t0)
        0x00001021u, // addu v0, zero, zero; disarm timer
        0x03e00008u, // jr ra
        0x00000000u
    };
    for (uint32_t i = 0u;
         i < sizeof(callback_words) / sizeof(callback_words[0]); ++i)
    {
        if (!kernel.cpu().write32(
                0x3000u + i * 4u, callback_words[i]))
        {
            return fail("cannot prepare original guest alarm callback");
        }
    }
    kernel.cpu().write32(0x2a20u, 1000u);
    kernel.cpu().write32(0x2a24u, 0u);
    kernel.cpu().write32(0x2a28u, 0u);
    if (!call_guest(
            kernel, "thbase", 35u,
            {0x2a20u, 0x3000u, 0u, 0u}, &result) ||
        result != 0u || !kernel.has_guest_work())
    {
        return fail("SetAlarm did not schedule a guest callback");
    }
    const iop::s_kernel_run_result timer_run = kernel.run(2048u);
    if (timer_run.m_stop != iop::e_kernel_stop::idle ||
        !kernel.cpu().read32(0x2a28u, &low) ||
        low != 1u || kernel.has_guest_work())
    {
        return fail("alarm callback did not execute and disarm");
    }

    kernel.cpu().write32(0x2a20u, 5000u);
    kernel.cpu().write32(0x2a28u, 0u);
    if (!call_guest(
            kernel, "thbase", 35u,
            {0x2a20u, 0x3000u, 0u, 0u}, &result) ||
        !call_guest(
            kernel, "thbase", 37u,
            {0x3000u, 0u, 0u, 0u}, &result) ||
        result != 0u || kernel.has_guest_work() ||
        !kernel.cpu().read32(0x2a28u, &low) || low != 0u)
    {
        return fail("CancelAlarm did not disarm the scheduled callback");
    }

    // Bounded guest-memory strncmp, lookup_ctype and strtol operate
    // through actual R3000 import stubs, not direct host-only calls.
    const std::string lhs = "ABC";
    const std::string rhs = "ABD";
    for (size_t i = 0u; i <= lhs.size(); ++i)
    {
        if (!kernel.cpu().write8(
                0x2200u + static_cast<uint32_t>(i),
                i == lhs.size() ? 0u :
                    static_cast<uint8_t>(lhs[i])) ||
            !kernel.cpu().write8(
                0x2300u + static_cast<uint32_t>(i),
                i == rhs.size() ? 0u :
                    static_cast<uint8_t>(rhs[i])))
        {
            return fail("cannot prepare guest strings");
        }
    }
    if (!call_guest(
            kernel, "sysclib", 29u,
            {0x2200u, 0x2300u, 3u, 0u}, &result) ||
        static_cast<int32_t>(result) != -1 ||
        !call_guest(
            kernel, "sysclib", 8u,
            {static_cast<uint32_t>('A'), 0u, 0u, 0u}, &result) ||
        (result & 1u) == 0u)
    {
        return fail("guest strncmp or character classification failed");
    }

    const std::string number = " -42foo";
    for (size_t i = 0u; i <= number.size(); ++i)
    {
        kernel.cpu().write8(
            0x2400u + static_cast<uint32_t>(i),
            i == number.size() ? 0u :
                static_cast<uint8_t>(number[i]));
    }
    uint32_t consumed = 0u;
    if (!call_guest(
            kernel, "sysclib", 36u,
            {0x2400u, 0x2500u, 10u, 0u}, &result) ||
        static_cast<int32_t>(result) != -42 ||
        !kernel.cpu().read32(0x2500u, &consumed) ||
        consumed != 0x2404u)
    {
        return fail("guest strtol or end pointer was incorrect");
    }

    // EventFlag and semaphore are distinct IOP-owned objects; they
    // cannot be conjured merely by the EE reading SIF flags.
    kernel.cpu().write32(0x2600u, 0u);
    kernel.cpu().write32(0x2604u, 0u);
    kernel.cpu().write32(0x2608u, 0u);
    if (!call_guest(
            kernel, "thevent", 4u,
            {0x2600u, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !call_guest(
            kernel, "thevent", 6u,
            {1u, 0x20u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thevent", 10u,
            {1u, 0x20u, 0u, 0x2620u}, &result) ||
        result != 0u)
    {
        return fail("guest event creation, signal or wait failed");
    }
    uint32_t event_bits = 0u;
    if (!kernel.cpu().read32(0x2620u, &event_bits) ||
        event_bits != 0x20u)
    {
        return fail("event flag did not report actual guest bits");
    }

    kernel.cpu().write32(0x2700u, 0u);
    kernel.cpu().write32(0x2704u, 0u);
    kernel.cpu().write32(0x2708u, 1u);
    kernel.cpu().write32(0x270cu, 2u);
    if (!call_guest(
            kernel, "thsemap", 4u,
            {0x2700u, 0u, 0u, 0u}, &result) ||
        result != 2u ||
        !call_guest(
            kernel, "thsemap", 8u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 6u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 11u,
            {2u, 0x2740u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("real guest semaphore calls did not complete");
    }
    uint32_t remaining = 0u;
    if (!kernel.cpu().read32(0x2750u, &remaining) ||
        remaining != 1u)
    {
        return fail("refer semaphore status reported false count");
    }

    // Polling does not block, status describes actual objects, and a
    // successfully destroyed object cannot be polled again.
    if (!call_guest(
            kernel, "thevent", 11u,
            {1u, 0x20u, 0u, 0x2660u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thevent", 12u,
            {1u, 0x40u, 0u, 0x2660u}, &result) ||
        result != static_cast<uint32_t>(-421) ||
        !call_guest(
            kernel, "thevent", 13u,
            {1u, 0x2680u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x268cu, &event_bits) ||
        event_bits != 0x20u ||
        !call_guest(
            kernel, "thevent", 9u,
            {1u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thevent", 14u,
            {1u, 0x2680u, 0u, 0u}, &result) ||
        !kernel.cpu().read32(0x268cu, &event_bits) ||
        event_bits != 0u ||
        !call_guest(
            kernel, "thevent", 5u,
            {1u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("event polling, status or delete failed");
    }
    if (!call_guest(
            kernel, "thsemap", 9u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 9u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-419) ||
        !call_guest(
            kernel, "thsemap", 7u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 12u,
            {2u, 0x2780u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2790u, &remaining) ||
        remaining != 1u ||
        !call_guest(
            kernel, "thsemap", 5u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("semaphore polling, status or delete failed");
    }

    // Cross-check the additional reference-backed families against
    // the actual guest-visible IOP allocation and thread state.
    uint32_t total_free = 0u;
    if (!call_guest(
            kernel, "sysmem", 6u, {0u, 0u, 0u, 0u}, &result) ||
        result != iop::c_cpu::k_ram_size ||
        !call_guest(
            kernel, "sysmem", 8u, {0u, 0u, 0u, 0u}, &result))
    {
        return fail("SYS_MEM capacity import failed");
    }
    total_free = result;
    if (!call_guest(
            kernel, "sysmem", 4u, {0u, 64u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("SYS_MEM guest allocation failed");
    }
    const uint32_t allocated = result;
    if (!call_guest(
            kernel, "sysmem", 5u,
            {allocated, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "sysmem", 7u, {0u, 0u, 0u, 0u}, &result) ||
        result == 0u ||
        !call_guest(
            kernel, "loadcore", 4u, {0u, 0u, 0u, 0u},
            &result) ||
        result != 0u ||
        !call_guest(
            kernel, "loadcore", 5u, {0u, 0u, 0u, 0u},
            &result) ||
        result != 0u ||
        !call_guest(
            kernel, "intrman", 23u, {0u, 0u, 0u, 0u},
            &result) ||
        result != 0u)
    {
        return fail("SYS_MEM free, coherent cache or context query failed");
    }
    // Each call_guest creates its own program thread stack, so total
    // free memory decreases across calls independently of FreeSysMemory.
    if (total_free == 0u)
    {
        return fail("SYS_MEM queried an impossible free-memory total");
    }

    kernel.cpu().write32(0x2b00u, 0u);
    kernel.cpu().write32(0x2b04u, 0u);
    kernel.cpu().write32(0x2b08u, 0x3000u);
    kernel.cpu().write32(0x2b0cu, 0x400u);
    kernel.cpu().write32(0x2b10u, 32u);
    if (!call_guest(
            kernel, "thbase", 4u,
            {0x2b00u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("guest thread creation import failed");
    }
    const uint32_t managed_thread = result;
    uint32_t thread_status = 0u;
    uint32_t priority = 0u;
    if (!call_guest(
            kernel, "thbase", 14u,
            {managed_thread, 16u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thbase", 22u,
            {managed_thread, 0x2b40u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2b48u, &thread_status) ||
        thread_status != 16u ||
        !kernel.cpu().read32(0x2b60u, &priority) ||
        priority != 16u ||
        !call_guest(
            kernel, "thbase", 26u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thbase", 27u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !call_guest(
            kernel, "thbase", 10u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thbase", 5u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 0u || kernel.thread(managed_thread) != nullptr)
    {
        return fail("THBASE guest thread management or status failed");
    }

    // Reference IOP mailbox and heaplib services are backed by real
    // queues and owned IOP guest allocations, not success-only stubs.
    kernel.cpu().write32(0x2c00u, 0u);
    kernel.cpu().write32(0x2c04u, 0u);
    kernel.cpu().write32(0x2c10u, 0u);
    if (!call_guest(
            kernel, "thmsgbx", 4u, {0x2c00u, 0u, 0u, 0u},
            &result) || result == 0u)
    {
        return fail("mailbox creation failed");
    }
    const uint32_t mailbox = result;
    if (!call_guest(
            kernel, "thmsgbx", 9u,
            {0x2c20u, mailbox, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-424) ||
        !call_guest(
            kernel, "thmsgbx", 6u,
            {mailbox, 0x2c10u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thmsgbx", 9u,
            {0x2c20u, mailbox, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2c20u, &remaining) ||
        remaining != 0x2c10u ||
        !call_guest(
            kernel, "thmsgbx", 5u,
            {mailbox, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("guest mailbox polling, delivery or deletion failed");
    }
    if (!call_guest(
            kernel, "heaplib", 4u, {0u, 0u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("heaplib create did not allocate a real object");
    }
    const uint32_t heap_id = result;
    if (!call_guest(
            kernel, "heaplib", 6u,
            {heap_id, 128u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base ||
        !kernel.owns_heap_span(result, 128u))
    {
        return fail("heaplib allocation did not reserve IOP memory");
    }
    const uint32_t block = result;
    if (!call_guest(
            kernel, "heaplib", 7u,
            {heap_id, block, 0u, 0u}, &result) ||
        result != 0u ||
        kernel.owns_heap_span(block, 1u) ||
        !call_guest(
            kernel, "heaplib", 5u,
            {heap_id, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        kernel.owns_heap_span(heap_id, 1u))
    {
        return fail("heaplib free or delete failed");
    }

    // A fixed pool only returns blocks it owns and rejects an
    // additional allocation after its last free slot is consumed.
    kernel.cpu().write32(0x2d00u, 0u);
    kernel.cpu().write32(0x2d04u, 0u);
    kernel.cpu().write32(0x2d08u, 16u);
    kernel.cpu().write32(0x2d0cu, 2u);
    if (!call_guest(
            kernel, "thfpool", 4u,
            {0x2d00u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("THFPOOL guest creation failed");
    }
    const uint32_t fixed_pool = result;
    if (!call_guest(
            kernel, "thfpool", 7u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("THFPOOL first allocation failed");
    }
    const uint32_t fixed_block_a = result;
    if (!call_guest(
            kernel, "thfpool", 8u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != fixed_block_a + 16u ||
        !call_guest(
            kernel, "thfpool", 7u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thfpool", 9u,
            {fixed_pool, fixed_block_a, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thfpool", 7u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != fixed_block_a ||
        !call_guest(
            kernel, "thfpool", 5u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("THFPOOL exhaustion, release or deletion failed");
    }
    kernel.cpu().write32(0x2e00u, 0u);
    kernel.cpu().write32(0x2e04u, 0u);
    kernel.cpu().write32(0x2e08u, 64u);
    if (!call_guest(
            kernel, "thvpool", 4u,
            {0x2e00u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("THVPOOL guest creation failed");
    }
    const uint32_t variable_pool = result;
    if (!call_guest(
            kernel, "thvpool", 7u,
            {variable_pool, 16u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("THVPOOL first allocation failed");
    }
    const uint32_t variable_block = result;
    if (!call_guest(
            kernel, "thvpool", 11u,
            {variable_pool, 0x2e40u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2e4cu, &remaining) ||
        remaining != 48u ||
        !call_guest(
            kernel, "thvpool", 9u,
            {variable_pool, variable_block, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thvpool", 12u,
            {variable_pool, 0x2e40u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2e4cu, &remaining) ||
        remaining != 64u ||
        !call_guest(
            kernel, "thvpool", 5u,
            {variable_pool, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("THVPOOL status, release or deletion failed");
    }

    // SYS_MEM block metadata comes from actual allocator ownership,
    // including interior pointers; non-owned addresses have no block.
    if (!call_guest(
            kernel, "sysmem", 4u,
            {0u, 64u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("second SYS_MEM allocation failed");
    }
    const uint32_t owned_block = result;
    if (!call_guest(
            kernel, "sysmem", 9u,
            {owned_block + 4u, 0u, 0u, 0u}, &result) ||
        result != owned_block ||
        !call_guest(
            kernel, "sysmem", 10u,
            {owned_block + 4u, 0u, 0u, 0u}, &result) ||
        result != 64u ||
        !call_guest(
            kernel, "sysmem", 5u,
            {owned_block, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "sysmem", 9u,
            {owned_block, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("SYS_MEM ownership/block metadata did not match");
    }

    const std::string strtok_input = "aaa,bb";
    for (size_t i = 0u; i <= strtok_input.size(); ++i)
    {
        if (!kernel.cpu().write8(
                0x2f00u + static_cast<uint32_t>(i),
                i == strtok_input.size() ? 0u :
                static_cast<uint8_t>(strtok_input[i])))
        {
            return fail("cannot initialize SYSCLIB token input");
        }
    }
    kernel.cpu().write8(0x2f40u, ',');
    kernel.cpu().write8(0x2f41u, 0u);
    if (!call_guest(
            kernel, "sysclib", 35u,
            {0x2f00u, 0x2f40u, 0u, 0u}, &result) ||
        result != 0x2f00u ||
        !call_guest(
            kernel, "sysclib", 35u,
            {0u, 0x2f40u, 0u, 0u}, &result) ||
        result != 0x2f04u ||
        !call_guest(
            kernel, "sysclib", 35u,
            {0u, 0x2f40u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("bounded SYSCLIB strtok continuation was incorrect");
    }
    const std::string unsigned_input = "4294967295tail";
    for (size_t i = 0u; i <= unsigned_input.size(); ++i)
    {
        if (!kernel.cpu().write8(
                0x2f60u + static_cast<uint32_t>(i),
                i == unsigned_input.size() ? 0u :
                static_cast<uint8_t>(unsigned_input[i])))
        {
            return fail("cannot initialize SYSCLIB unsigned input");
        }
    }
    uint32_t unsigned_end = 0u;
    if (!call_guest(
            kernel, "sysclib", 38u,
            {0x2f60u, 0x2f90u, 10u, 0u}, &result) ||
        result != 0xffffffffu ||
        !kernel.cpu().read32(0x2f90u, &unsigned_end) ||
        unsigned_end != 0x2f6au ||
        !call_guest(
            kernel, "sysclib", 41u,
            {0x2fa0u, 0x12345678u, 8u, 0u}, &result) ||
        result != 0x2fa0u ||
        !call_guest(
            kernel, "sysclib", 40u,
            {0x2fb0u, 0x2fa0u, 8u, 0u}, &result) ||
        result != 0x2fb0u ||
        !kernel.cpu().read32(0x2fb0u, &remaining) ||
        remaining != 0x12345678u ||
        !kernel.cpu().read32(0x2fb4u, &remaining) ||
        remaining != 0x12345678u)
    {
        return fail("SYSCLIB strtoul or word memory operations failed");
    }


    // The real CRI_ADXI IRX imports SYSCLIB sprintf #19. Its
    // first variable arguments occupy O32 a2/a3, with further
    // arguments in the guest stack; vsprintf #42 receives a guest
    // va_list. Never pass guest pointers to the host formatter.
    const auto write_guest_string =
        [&kernel](uint32_t address, const std::string& value)
    {
        for (uint32_t i = 0u; i <= value.size(); ++i)
        {
            if (!kernel.cpu().write8(
                    address + i,
                    i == value.size() ? 0u :
                        static_cast<uint8_t>(value[i])))
            {
                return false;
            }
        }
        return true;
    };
    const auto check_guest_string =
        [&kernel](uint32_t address, const std::string& value)
    {
        for (uint32_t i = 0u; i <= value.size(); ++i)
        {
            uint8_t actual = 0xffu;
            if (!kernel.cpu().read8(address + i, &actual) ||
                actual != (i == value.size() ? 0u :
                           static_cast<uint8_t>(value[i])))
            {
                return false;
            }
        }
        return true;
    };

    constexpr uint32_t k_sprintf_destination = 0x3400u;
    constexpr uint32_t k_sprintf_format = 0x3480u;
    constexpr uint32_t k_sprintf_string = 0x34c0u;
    constexpr uint32_t k_vsprintf_format = 0x3500u;
    constexpr uint32_t k_vsprintf_argument_list = 0x3580u;
    constexpr uint32_t k_vsprintf_destination = 0x3600u;

    const std::string expected_sprintf = "mod=002A/audio";
    if (!write_guest_string(
            k_sprintf_format, "mod=%04X/%s") ||
        !write_guest_string(k_sprintf_string, "audio") ||
        !call_guest(
            kernel, "sysclib", 19u,
            {k_sprintf_destination, k_sprintf_format,
             0x2au, k_sprintf_string}, &result) ||
        result != expected_sprintf.size() ||
        !check_guest_string(
            k_sprintf_destination, expected_sprintf))
    {
        return fail("guest SYSCLIB sprintf O32 a2/a3 formatting failed");
    }

    const std::string expected_vsprintf = "00000012:-7:audio";
    if (!write_guest_string(
            k_vsprintf_format, "%08x:%d:%s") ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list, 0x12u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 4u, 0xfffffff9u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 8u, k_sprintf_string) ||
        !call_guest(
            kernel, "sysclib", 42u,
            {k_vsprintf_destination, k_vsprintf_format,
             k_vsprintf_argument_list, 0u}, &result) ||
        result != expected_vsprintf.size() ||
        !check_guest_string(
            k_vsprintf_destination, expected_vsprintf))
    {
        return fail("guest SYSCLIB vsprintf guest va_list was incorrect");
    }

    // The O32 guest va_list aligns 64-bit integer/double arguments
    // and preserves their little-endian words.
    const std::string expected_wide = "-42/1.5";
    if (!write_guest_string(k_vsprintf_format, "%lld/%.1f") ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list, 0xffffffd6u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 4u, 0xffffffffu) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 8u, 0u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 12u, 0x3ff80000u) ||
        !call_guest(
            kernel, "sysclib", 42u,
            {k_vsprintf_destination, k_vsprintf_format,
             k_vsprintf_argument_list, 0u}, &result) ||
        result != expected_wide.size() ||
        !check_guest_string(k_vsprintf_destination, expected_wide))
    {
        return fail("guest SYSCLIB 64-bit varargs were misaligned");
    }

    // Malformed or dangerous format strings must reject the whole
    // request before changing any destination byte or guest result.
    for (const std::string& unsafe : {
             std::string("bad%n"), std::string("%5000d"),
             std::string("dangling%")})
    {
        if (!write_guest_string(k_sprintf_format, unsafe) ||
            !kernel.cpu().write32(
                k_sprintf_destination, 0x55555555u))
        {
            return fail("cannot prepare rejected sprintf test");
        }
        kernel.cpu().state().m_gpr[4] = k_sprintf_destination;
        kernel.cpu().state().m_gpr[5] = k_sprintf_format;
        kernel.cpu().state().m_gpr[6] = k_sprintf_string;
        const uint32_t prior = kernel.cpu().state().m_gpr[2];
        uint32_t preserved = 0u;
        if (iop::execute_sysclib_format(kernel.cpu(), 19u) ||
            !kernel.cpu().read32(
                k_sprintf_destination, &preserved) ||
            preserved != 0x55555555u ||
            kernel.cpu().state().m_gpr[2] != prior)
        {
            return fail("rejected SYSCLIB sprintf modified guest RAM");
        }
    }

    // StartThreadArgs preserves both guest entry arguments; the new
    // thread executes actual R3000 instructions when scheduled.
    kernel.cpu().write32(0x3e00u, 0x0000000du);
    const uint32_t args_thread =
        kernel.create_thread(0x3e00u, 0u, 32u, 0x400u);
    if (args_thread == 0u ||
        !call_guest(
            kernel, "thbase", 7u,
            {args_thread, 0x80u, 0x2e00u, 0u}, &result) ||
        result != 0u ||
        kernel.thread(args_thread) == nullptr ||
        kernel.thread(args_thread)->m_cpu.m_gpr[4] != 0x80u ||
        kernel.thread(args_thread)->m_cpu.m_gpr[5] != 0x2e00u)
    {
        return fail("StartThreadArgs lost guest argument state");
    }
    const iop::s_kernel_run_result args_run = kernel.run(64u);
    if (args_run.m_stop != iop::e_kernel_stop::breakpoint ||
        args_run.m_guest_pc != 0x3e00u)
    {
        return fail("StartThreadArgs guest was not actually scheduled");
    }

    // ExitDeleteThread must retire the real guest thread/stack after
    // its syscall returns; an implementation that merely reports zero
    // would instead execute the following BREAK.
    uint32_t exit_import = 0u;
    if (!kernel.lookup_library("thbase", 9u, &exit_import) ||
        !kernel.cpu().write32(
            0x3f40u, 0x0c000000u |
                ((exit_import >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(0x3f44u, 0u) ||
        !kernel.cpu().write32(0x3f48u, 0x0000000du))
    {
        return fail("cannot prepare ExitDeleteThread guest");
    }
    const uint32_t delete_thread =
        kernel.create_thread(0x3f40u, 0u, 32u, 0x400u);
    if (delete_thread == 0u ||
        !kernel.start_thread(delete_thread))
    {
        return fail("cannot schedule ExitDeleteThread guest");
    }
    const iop::s_kernel_run_result delete_run = kernel.run(128u);
    if (delete_run.m_stop != iop::e_kernel_stop::idle ||
        kernel.thread(delete_thread) != nullptr)
    {
        return fail("ExitDeleteThread did not retire the real thread");
    }

    // SIO2 DMA MMIO request retains STR, but never invents a DMA
    // completion or sets IOP interrupt readiness.
    uint32_t request = 0u;
    if (!call_guest(
            kernel, "dmacman", 32u,
            {11u, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !kernel.hardware().read(
            iop::c_hw::k_sio2_dma_rx_base + 8u,
            4u, &request) ||
        request != 0x01000200u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("SIO2 DMA request was not hardware-backed");
    }

    // Unknown IOP service must remain unhandled; reset invalidates
    // all installed imports and hosted semaphore/event state.
    kernel.reset();
    hle.reset_for_reboot();
    uint32_t stale = 0u;
    if (kernel.lookup_library("loadcore", 6u, &stale) ||
        !hle.install() ||
        !kernel.lookup_library("loadcore", 6u, &stale))
    {
        return fail("reboot did not reinstall legitimate HLE libraries");
    }

    std::printf("IOP kernel HLE service families: PASS\n");
    return 0;
}
