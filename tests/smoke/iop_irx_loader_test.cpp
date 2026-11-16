#include "irx_loader.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
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

int fail(const char* message)
{
    std::fprintf(stderr, "IOP IRX loader: %s\n", message);
    return 1;
}

std::vector<uint8_t> make_irx(uint16_t type)
{
    std::vector<uint8_t> elf(0x240u, 0u);
    elf[0] = 0x7fu;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1u;
    elf[5] = 1u;
    elf[6] = 1u;
    put16(elf, 16u, type);
    put16(elf, 18u, 8u);     // EM_MIPS
    put32(elf, 28u, 0x34u);  // e_phoff
    put32(elf, 32u, 0xc0u);  // e_shoff
    put16(elf, 40u, 52u);    // e_ehsize
    put16(elf, 42u, 32u);    // e_phentsize
    put16(elf, 44u, 2u);     // e_phnum
    put16(elf, 46u, 40u);    // e_shentsize
    put16(elf, 48u, 1u);     // e_shnum

    // PT_LOAD: copy 96 bytes from 0x100, zero-fill through 0x180.
    put32(elf, 0x34u, 1u);
    put32(elf, 0x38u, 0x100u);
    put32(elf, 0x44u, 0x60u);
    put32(elf, 0x48u, 0x80u);

    // PT_SCE_IOPMOD: 4-byte header, entry/gp, text/data/bss, name.
    put32(elf, 0x54u, 0x70000080u);
    put32(elf, 0x58u, 0x180u);
    put32(elf, 0x184u, 0x10u); // module entry offset
    put32(elf, 0x188u, 0x50u); // gp offset
    put32(elf, 0x18cu, 0x40u);
    put32(elf, 0x190u, 0x20u);
    put32(elf, 0x194u, 0x20u);
    const char name[] = "TESTMOD";
    for (uint32_t index = 0u; index < sizeof(name); ++index)
    {
        elf[0x19au + index] = static_cast<uint8_t>(name[index]);
    }

    put32(elf, 0x100u, 0x00000024u); // R_MIPS_32
    put32(elf, 0x104u, 0x08000010u); // R_MIPS_26
    put32(elf, 0x108u, 0x3c081234u); // R_MIPS_HI16
    put32(elf, 0x10cu, 0x25088000u); // R_MIPS_LO16
    put32(elf, 0x110u, 0x24020003u); // addiu v0,zero,3
    put32(elf, 0x114u, 0x24420004u); // addiu v0,v0,4
    put32(elf, 0x118u, 0x0000000du); // break

    // IOP import table: LIBRARY #7, explicit resolve via kernel.
    put32(elf, 0x120u, 0x41e00000u);
    const char library[] = "LIBRARY";
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        elf[0x12cu + index] = static_cast<uint8_t>(library[index]);
    }
    put32(elf, 0x134u, 0x03e00008u);
    put32(elf, 0x138u, 0x24000007u);

    // One SHT_REL section containing four in-place relocations.
    put32(elf, 0xc4u, 9u);
    put32(elf, 0xd0u, 0x200u);
    put32(elf, 0xd4u, 4u * 8u);
    const uint32_t types[] = {2u, 4u, 5u, 6u};
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        put32(elf, 0x200u + 8u * index, index * 4u);
        put32(elf, 0x204u + 8u * index, types[index]);
    }
    return elf;
}
} // namespace

int main()
{
    constexpr uint32_t base = 0x20000u;
    constexpr uint32_t marker = 0xcafebabeu;
    iop::c_cpu cpu;
    iop::s_irx_module module{};
    module.m_name = "unchanged";

    auto elf = make_irx(0xff80u);
    if (!cpu.write32(base, marker))
    {
        return fail("cannot prepare IOP RAM");
    }
    if (iop::load_irx(cpu, elf, base, &module) !=
        iop::e_irx_load_result::unresolved_import)
    {
        return fail("unknown IOP import should fail closed");
    }

    uint32_t value = 0u;
    if (!cpu.read32(base, &value) || value != marker ||
        module.m_name != "unchanged")
    {
        return fail("failed loader mutated memory or output metadata");
    }

    const auto resolver = [](
        const std::string& library, uint16_t ordinal, uint32_t* target
    ) -> bool
    {
        if (library != "LIBRARY" || ordinal != 7u ||
            target == nullptr)
        {
            return false;
        }
        *target = 0x24000u;
        return true;
    };
    if (iop::load_irx(cpu, elf, base, &module, resolver) !=
        iop::e_irx_load_result::success ||
        module.m_name != "TESTMOD" ||
        module.m_base != base ||
        module.m_size != 0x80u ||
        module.m_entry != base + 0x10u ||
        module.m_gp != base + 0x50u)
    {
        return fail("valid IRX load or module metadata");
    }

    const uint32_t expected[] =
    {
        0x00020024u, 0x08008010u,
        0x3c081236u, 0x25088000u
    };
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        if (!cpu.read32(base + 4u * index, &value) ||
            value != expected[index])
        {
            return fail("IRX relocation differs from MIPS linker semantics");
        }
    }
    if (!cpu.read32(base + 0x34u, &value) ||
        value != 0x08009000u ||
        !cpu.read32(base + 0x38u, &value) || value != 0u ||
        !cpu.read32(base + 0x7cu, &value) || value != 0u)
    {
        return fail("import stub linking or BSS zero-fill");
    }

    cpu.state().m_pc = module.m_entry;
    cpu.state().m_next_pc = module.m_entry + 4u;
    const iop::s_run_result run = cpu.run(8u);
    if (run.m_result != iop::e_step_result::breakpoint ||
        run.m_instructions != 2u ||
        cpu.state().m_gpr[2] != 7u)
    {
        return fail("loaded IRX entry is not executable R3000 code");
    }

    // Variant 2 has a different HI16 relocation convention; both
    // variants must generate the same instruction words in this fixture.
    auto elf_v2 = make_irx(0xff81u);
    iop::c_cpu other_cpu;
    iop::s_irx_module other_module{};
    if (iop::load_irx(
            other_cpu, elf_v2, base, &other_module, resolver
        ) != iop::e_irx_load_result::success)
    {
        return fail("version-2 IOP IRX relocation");
    }
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        if (!other_cpu.read32(base + 4u * index, &value) ||
            value != expected[index])
        {
            return fail("version-2 R_MIPS_HI16/LO16 relocation");
        }
    }

    // Malformed relocation offsets and unsupported types are rejected
    // before *any* IOP RAM is overwritten.
    const uint32_t sentinel_location = base + 0x1000u;
    for (uint32_t invalid = 0u; invalid < 3u; ++invalid)
    {
        auto broken = elf;
        switch (invalid)
        {
        case 0u:
            put32(broken, 0x200u, 0x1000u);
            break;
        case 1u:
            put32(broken, 0x204u, 0x00000007u);
            break;
        case 2u:
            put32(broken, 0xd4u, 0xffffffffu);
            break;
        }
        if (!cpu.write32(sentinel_location, marker))
        {
            return fail("cannot prepare rollback sentinel");
        }
        iop::s_irx_module unchanged{};
        unchanged.m_name = "KEEP";
        const iop::e_irx_load_result result = iop::load_irx(
            cpu, broken, sentinel_location, &unchanged, resolver
        );
        if (result == iop::e_irx_load_result::success ||
            !cpu.read32(sentinel_location, &value) ||
            value != marker || unchanged.m_name != "KEEP")
        {
            return fail("invalid IRX partially modified guest RAM");
        }
    }

    const auto bad_target = [](
        const std::string&, uint16_t, uint32_t* target
    ) -> bool
    {
        *target = 0x80024000u;
        return true;
    };
    if (iop::load_irx(cpu, elf, sentinel_location,
                      &module, bad_target) !=
        iop::e_irx_load_result::invalid_import_target)
    {
        return fail("unencodable cross-region IOP jump accepted");
    }

    auto no_memory = make_irx(0xff80u);
    if (iop::load_irx(
            cpu, no_memory, iop::c_cpu::k_ram_size - 64u,
            &module, resolver
        ) != iop::e_irx_load_result::insufficient_ram)
    {
        return fail("out-of-bounds module allocation accepted");
    }

    std::printf("IOP IRX loader: PASS\n");
    return 0;
}
