#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
void put16(std::vector<uint8_t>& data, uint32_t at, uint16_t value)
{
    data[at] = static_cast<uint8_t>(value);
    data[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& data, uint32_t at, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[at + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

int fail(const char* reason)
{
    std::fprintf(stderr, "IOP IRX export linker: %s\n", reason);
    return 1;
}

std::vector<uint8_t> make_module(bool provider)
{
    // Minimal synthetic relocatable IOP ELF with PT_SCE_IOPMOD.
    std::vector<uint8_t> bytes(0x240u, 0u);
    bytes[0] = 0x7fu;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1u;
    bytes[5] = 1u;
    bytes[6] = 1u;
    put16(bytes, 16u, 0xff80u);
    put16(bytes, 18u, 8u);
    put32(bytes, 28u, 0x34u);
    put32(bytes, 32u, 0x1c0u);
    put16(bytes, 40u, 52u);
    put16(bytes, 42u, 32u);
    put16(bytes, 44u, 2u);
    put16(bytes, 46u, 40u);
    put16(bytes, 48u, 1u);

    put32(bytes, 0x34u, 1u);
    put32(bytes, 0x38u, 0x100u);
    put32(bytes, 0x44u, 0x80u);
    put32(bytes, 0x48u, 0x90u);

    put32(bytes, 0x54u, 0x70000080u);
    put32(bytes, 0x58u, 0x180u);
    put32(bytes, 0x184u, 0u);
    put32(bytes, 0x188u, 0x50u);
    put32(bytes, 0x18cu, 0x70u);
    put32(bytes, 0x190u, 0x10u);
    put32(bytes, 0x194u, 0x10u);
    const char* name = provider ? "PROVIDER" : "CONSUMER";
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        bytes[0x19au + i] = static_cast<uint8_t>(name[i]);
    }

    if (provider)
    {
        // A real callable R3000 function at module+0x10.
        put32(bytes, 0x110u, 0x2402002au); // addiu v0,zero,42
        put32(bytes, 0x114u, 0x03e00008u); // jr ra
        put32(bytes, 0x118u, 0u);         // delay slot

        // IOP export library PROVIDER, ordinal 0 => function+0x10.
        put32(bytes, 0x120u, 0x41c00000u);
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            bytes[0x12cu + i] =
                static_cast<uint8_t>("PROVIDER"[i]);
        }
        put32(bytes, 0x134u, 0x10u);
        put32(bytes, 0x138u, 0u);
    }
    else
    {
        put32(bytes, 0x100u, 0x0c00000du); // jal module+0x34
        put32(bytes, 0x104u, 0u);         // delay slot
        put32(bytes, 0x108u, 0x0000000du); // break
        put32(bytes, 0x120u, 0x41e00000u);
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            bytes[0x12cu + i] =
                static_cast<uint8_t>("PROVIDER"[i]);
        }
        put32(bytes, 0x134u, 0x03e00008u); // jr ra
        put32(bytes, 0x138u, 0x24000000u); // import ordinal 0
    }

    // Relocate the provider's export pointer or consumer's JAL
    // target with MIPS R_32 / R_26 respectively.
    put32(bytes, 0x1c4u, 9u);
    put32(bytes, 0x1d0u, 0x210u);
    put32(bytes, 0x1d4u, 8u);
    put32(bytes, 0x210u, provider ? 0x34u : 0u);
    put32(bytes, 0x214u, provider ? 2u : 4u);
    return bytes;
}
} // namespace

int main()
{
    constexpr uint32_t k_base = 0x20000u;
    constexpr uint32_t k_module_size = 0x90u;
    constexpr uint32_t k_sentinel = 0xfaceb00cu;
    iop::c_kernel kernel;
    const std::vector<uint8_t> provider = make_module(true);
    const std::vector<uint8_t> consumer = make_module(false);

    // A dependent module cannot be linked before the provider exists.
    const iop::s_module_load_result early = kernel.load_module(consumer);
    if (early.m_status != iop::e_irx_load_result::unresolved_import ||
        early.m_id != 0u)
    {
        return fail("consumer import unexpectedly resolved before provider");
    }

    const iop::s_module_load_result exported = kernel.load_module(provider);
    const iop::s_kernel_module* module = kernel.module(exported.m_id);
    if (exported.m_status != iop::e_irx_load_result::success ||
        exported.m_id != 1u || module == nullptr ||
        module->m_irx.m_base != k_base ||
        module->m_irx.m_size != k_module_size ||
        module->m_irx.m_exports.size() != 1u ||
        module->m_irx.m_exports[0].m_library != "PROVIDER" ||
        module->m_irx.m_exports[0].m_ordinal != 0u ||
        module->m_irx.m_exports[0].m_entry != k_base + 0x10u)
    {
        return fail("relocated provider export table not registered");
    }

    uint32_t word = 0u;
    if (!kernel.cpu().read32(k_base + 0x34u, &word) ||
        word != k_base + 0x10u)
    {
        return fail("exported function pointer was not relocated");
    }

    // A rejected conflicting image must not overwrite the next
    // allocation or consume a module ID.
    if (!kernel.cpu().write32(k_base + k_module_size, k_sentinel))
    {
        return fail("could not install rollback sentinel");
    }
    const iop::s_module_load_result duplicate = kernel.load_module(provider);
    if (duplicate.m_status != iop::e_irx_load_result::duplicate_export ||
        duplicate.m_id != 0u ||
        !kernel.cpu().read32(k_base + k_module_size, &word) ||
        word != k_sentinel)
    {
        return fail("duplicate library export did not fail before RAM commit");
    }

    // Bad exported pointers must also fail without modifying IOP memory.
    std::vector<uint8_t> malformed = provider;
    put32(malformed, 0x134u, 0x12345678u);
    const iop::s_module_load_result malformed_result =
        kernel.load_module(malformed);
    if (malformed_result.m_status != iop::e_irx_load_result::invalid_export ||
        !kernel.cpu().read32(k_base + k_module_size, &word) ||
        word != k_sentinel)
    {
        return fail("malformed export target was accepted or corrupted RAM");
    }

    // Now the kernel resolves the consumer's import against the
    // real, relocated guest address registered by the provider.
    const iop::s_module_load_result linked = kernel.load_module(consumer);
    const iop::s_kernel_module* caller = kernel.module(linked.m_id);
    if (linked.m_status != iop::e_irx_load_result::success ||
        linked.m_id != 2u || caller == nullptr ||
        caller->m_irx.m_base != k_base + k_module_size)
    {
        return fail("provider exports did not link consumer imports");
    }

    const uint32_t guest_stub = caller->m_irx.m_base + 0x34u;
    if (!kernel.cpu().read32(guest_stub, &word) ||
        word != (0x08000000u | ((k_base + 0x10u) >> 2u)))
    {
        return fail("consumer import does not target provider function");
    }

    const uint32_t thread_id = kernel.create_module_thread(linked.m_id, 1u);
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return fail("failed to start consumer IRX thread");
    }
    const iop::s_kernel_run_result run = kernel.run(32u);
    const iop::s_kernel_thread* thread = kernel.thread(thread_id);
    if (run.m_stop != iop::e_kernel_stop::breakpoint ||
        run.m_guest_pc != caller->m_irx.m_entry + 8u ||
        thread == nullptr || thread->m_cpu.m_gpr[2] != 42u)
    {
        return fail("cross-module guest R3000 call did not execute");
    }

    // A preinstalled HLE export cannot be silently overwritten by
    // a module loaded later.
    iop::c_kernel conflicting;
    if (!conflicting.register_library("PROVIDER", 0u, k_base + 0x10u) ||
        !conflicting.cpu().write32(k_base, k_sentinel))
    {
        return fail("cannot establish conflicting export fixture");
    }
    const iop::s_module_load_result hle_conflict =
        conflicting.load_module(provider);
    if (hle_conflict.m_status != iop::e_irx_load_result::duplicate_export ||
        !conflicting.cpu().read32(k_base, &word) || word != k_sentinel)
    {
        return fail("guest export overwrote an explicitly registered HLE");
    }

    std::printf("IOP IRX export linker: PASS\n");
    return 0;
}
