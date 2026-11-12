#include "ee_decoder.h"
#include "ee_instruction.h"
#include "ee_opcode.h"
#include "function_analyzer.h"
#include "elf_loader.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool value, const char* name)
{
    if (value)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("FAIL: %s\n", name);
    }
}

uint32_t encode_i(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t immediate)
{
    return (opcode << 26) | (rs << 21) | (rt << 16) | immediate;
}

uint32_t encode_lui(uint32_t rt, uint16_t immediate)
{
    return encode_i(0x0f, 0, rt, immediate);
}

uint32_t encode_ori(uint32_t rs, uint32_t rt, uint16_t immediate)
{
    return encode_i(0x0d, rs, rt, immediate);
}

uint32_t encode_addiu(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x09, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_lw(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x23, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_lwu(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x27, rs, rt, static_cast<uint16_t>(immediate));
}

void write_le16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write_le32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> make_static_data_elf(bool relocate_readonly_word = false)
{
    // Minimal ET_EXEC with one executable segment, one read-only data segment,
    // one writable data segment, and a valid section table. Each data segment
    // stores the same target; the optional SHT_REL relocates the read-only word.
    std::vector<uint8_t> bytes(0x270, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1; // ELFCLASS32
    bytes[5] = 1; // little endian
    bytes[6] = 1; // ELF version

    write_le16(bytes, 16, 2);  // ET_EXEC
    write_le16(bytes, 18, 8);  // EM_MIPS
    write_le32(bytes, 20, 1);
    write_le32(bytes, 24, 0x1000);
    write_le32(bytes, 28, 52); // program header table
    write_le32(bytes, 32, 0x220); // section header table
    write_le16(bytes, 40, 52);
    write_le16(bytes, 42, 32);
    write_le16(bytes, 44, 3);
    write_le16(bytes, 46, 40);
    write_le16(bytes, 48, relocate_readonly_word ? 2 : 1);
    write_le16(bytes, 50, 0);

    auto write_segment = [&](size_t header, uint32_t offset,
                             uint32_t address, uint32_t filesz,
                             uint32_t memsz, uint32_t flags)
    {
        write_le32(bytes, header + 0, 1); // PT_LOAD
        write_le32(bytes, header + 4, offset);
        write_le32(bytes, header + 8, address);
        write_le32(bytes, header + 12, address);
        write_le32(bytes, header + 16, filesz);
        write_le32(bytes, header + 20, memsz);
        write_le32(bytes, header + 24, flags);
        write_le32(bytes, header + 28, 4);
    };

    write_segment(52, 0x100, 0x1000, 0x40, 0x40, 5); // PF_R | PF_X
    write_segment(84, 0x200, 0x2000, 4, 8, 4);        // PF_R, file + BSS
    write_segment(116, 0x210, 0x3000, 4, 4, 6);       // PF_R | PF_W

    write_le32(bytes, 0x200, 0x00001020);
    write_le32(bytes, 0x210, 0x00001020);

    if (relocate_readonly_word)
    {
        // Second section header is SHT_REL for the read-only pointer word.
        const size_t section = 0x248;
        write_le32(bytes, section + 4, 9);      // SHT_REL
        write_le32(bytes, section + 16, 0x260); // relocation entries
        write_le32(bytes, section + 20, 8);     // one Elf32_Rel
        write_le32(bytes, section + 36, 8);     // Elf32_Rel size
        write_le32(bytes, 0x260, 0x2000);       // relocated word address
        write_le32(bytes, 0x264, 0x00000102);   // relocation info
    }
    return bytes;
}

uint32_t encode_beq(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x04, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_beql(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x14, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_jr(uint32_t rs)
{
    return (rs << 21) |
           static_cast<uint32_t>(ee::e_ee_opcode::FUNCT_JR);
}

uint32_t encode_jalr(uint32_t rs, uint32_t rd = 31)
{
    return (rs << 21) | (rd << 11) |
           static_cast<uint32_t>(ee::e_ee_opcode::FUNCT_JALR);
}

void add(std::map<uint32_t, ee::s_ee_instruction>& insns,
         uint32_t pc, uint32_t word)
{
    insns[pc] = ee::decode(pc, word);
}

std::set<uint32_t> recover(
    const std::map<uint32_t, ee::s_ee_instruction>& insns,
    uint32_t entry,
    const ee::elf_loader* memory_image = nullptr
)
{
    return ee::collect_static_jalr_targets(
        insns, {entry}, {entry}, nullptr, nullptr, memory_image
    );
}

std::vector<ee::s_static_jalr_site> audit_jalr_sites(
    const std::map<uint32_t, ee::s_ee_instruction>& insns,
    uint32_t entry
)
{
    std::vector<ee::s_static_jalr_site> sites;
    ee::collect_static_jalr_targets(
        insns, {entry}, {entry}, nullptr, &sites
    );
    return sites;
}
} // namespace

int main()
{
    // Canonical absolute-address sequence: LUI + ORI + JALR.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1000, encode_lui(25, 0x0012));
        add(insns, 0x1004, encode_ori(25, 25, 0x3454));
        add(insns, 0x1008, encode_jalr(25));
        add(insns, 0x100c, 0); // delay slot
        const auto targets = recover(insns, 0x1000);
        check(targets == std::set<uint32_t>{0x00123454u},
              "recover LUI/ORI-built JALR target");
    }

    // Signed low-half address adjustment commonly used with %hi/%lo.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1100, encode_lui(25, 0x0013));
        add(insns, 0x1104, encode_addiu(25, 25, -4));
        add(insns, 0x1108, encode_jalr(25));
        add(insns, 0x110c, 0);
        const auto targets = recover(insns, 0x1100);
        check(targets == std::set<uint32_t>{0x0012fffcu},
              "recover LUI/ADDIU signed-low target");
    }

    // Constant $zero is a valid source for a small absolute address.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1200, encode_ori(0, 25, 0x1800));
        add(insns, 0x1204, encode_jalr(25));
        add(insns, 0x1208, 0);
        const auto targets = recover(insns, 0x1200);
        check(targets == std::set<uint32_t>{0x1800u},
              "propagate the architectural zero register");
    }

    // The target must be sampled at JALR, before its delay slot executes.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1300, encode_lui(25, 0x0010));
        add(insns, 0x1304, encode_ori(25, 25, 0x3000));
        add(insns, 0x1308, encode_jalr(25));
        add(insns, 0x130c, encode_ori(0, 25, 0x4000));
        const auto targets = recover(insns, 0x1300);
        check(targets == std::set<uint32_t>{0x00103000u},
              "capture JALR target before delay-slot register writes");
    }

    // A load into the target register destroys the static value.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1400, encode_lui(25, 0x0010));
        add(insns, 0x1404, encode_lw(8, 25, 0));
        add(insns, 0x1408, encode_jalr(25));
        add(insns, 0x140c, 0);
        const auto targets = recover(insns, 0x1400);
        check(targets.empty(),
              "do not infer target after an unknown memory load");
    }

    // A file-backed read from a readable, non-writable ET_EXEC segment can
    // remain constant through LW and make a table-based JALR target static.
    {
        const std::vector<uint8_t> elf_bytes = make_static_data_elf();
        ee::elf_loader image;
        std::string error;
        const bool loaded = image.load_buffer(
            elf_bytes.data(), elf_bytes.size(), &error
        );
        uint32_t word = 0;
        check(loaded && image.read32_const_at(0x2000, &word) &&
              word == 0x00001020u,
              "read file-backed word from immutable ELF segment");

        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1000, encode_ori(0, 8, 0x2000));
        add(insns, 0x1004, encode_lw(8, 25, 0));
        add(insns, 0x1008, encode_jalr(25));
        add(insns, 0x100c, 0);
        const auto targets = recover(insns, 0x1000, &image);
        check(targets == std::set<uint32_t>{0x00001020u},
              "recover JALR target loaded from immutable ELF data");
    }

    // Writable globals are never treated as constants, even when their
    // on-disk contents look like valid function pointers.
    {
        const std::vector<uint8_t> elf_bytes = make_static_data_elf();
        ee::elf_loader image;
        std::string error;
        const bool loaded = image.load_buffer(
            elf_bytes.data(), elf_bytes.size(), &error
        );
        uint32_t word = 0;
        check(loaded && !image.read32_const_at(0x3000, &word),
              "reject pointer data in writable ELF segment");

        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1100, encode_ori(0, 8, 0x3000));
        add(insns, 0x1104, encode_lw(8, 25, 0));
        add(insns, 0x1108, encode_jalr(25));
        add(insns, 0x110c, 0);
        check(recover(insns, 0x1100, &image).empty(),
              "keep writable-global JALR target dynamic");
    }

    // A read-only word with an ELF relocation is not immutable at runtime.
    {
        const std::vector<uint8_t> elf_bytes = make_static_data_elf(true);
        ee::elf_loader image;
        std::string error;
        const bool loaded = image.load_buffer(
            elf_bytes.data(), elf_bytes.size(), &error
        );
        uint32_t word = 0;
        check(loaded && !image.read32_const_at(0x2000, &word),
              "reject relocated pointer words in read-only ELF segments");

        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1180, encode_ori(0, 8, 0x2000));
        add(insns, 0x1184, encode_lw(8, 25, 0));
        add(insns, 0x1188, encode_jalr(25));
        add(insns, 0x118c, 0);
        check(recover(insns, 0x1180, &image).empty(),
              "keep relocation-derived JALR target dynamic");
    }

    // The memory tail of a PT_LOAD segment is BSS, not file-backed data.
    // It must not be mistaken for the zero-filled bytes in the current ELF.
    {
        const std::vector<uint8_t> elf_bytes = make_static_data_elf();
        ee::elf_loader image;
        std::string error;
        const bool loaded = image.load_buffer(
            elf_bytes.data(), elf_bytes.size(), &error
        );
        uint32_t word = 0;
        check(loaded && !image.read32_const_at(0x2004, &word),
              "reject uninitialized BSS words from static memory analysis");

        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1200, encode_ori(0, 8, 0x2004));
        add(insns, 0x1204, encode_lw(8, 25, 0));
        add(insns, 0x1208, encode_jalr(25));
        add(insns, 0x120c, 0);
        check(recover(insns, 0x1200, &image).empty(),
              "keep BSS-derived JALR target unknown");
    }

    // Two CFG paths reaching JALR with different constants must merge to
    // unknown; seeing either individual path is not enough to report a target.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1000, encode_beq(1, 2, 5)); // target 0x1018
        add(insns, 0x1004, 0);
        add(insns, 0x1008, encode_lui(25, 0x0010));
        add(insns, 0x100c, encode_ori(25, 25, 0x1000));
        add(insns, 0x1010, encode_beq(0, 0, 3)); // target 0x1020
        add(insns, 0x1014, 0);
        add(insns, 0x1018, encode_lui(25, 0x0010));
        add(insns, 0x101c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1020, encode_jalr(25));
        add(insns, 0x1024, 0);
        const auto targets = recover(insns, 0x1000);
        check(targets.empty(),
              "discard JALR target when CFG predecessors disagree");
    }

    // Matching constants on every incoming path remain recoverable.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1500, encode_beq(1, 2, 5)); // target 0x1518
        add(insns, 0x1504, 0);
        add(insns, 0x1508, encode_lui(25, 0x0010));
        add(insns, 0x150c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1510, encode_beq(0, 0, 3)); // target 0x1520
        add(insns, 0x1514, 0);
        add(insns, 0x1518, encode_lui(25, 0x0010));
        add(insns, 0x151c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1520, encode_jalr(25));
        add(insns, 0x1524, 0);
        const auto targets = recover(insns, 0x1500);
        check(targets == std::set<uint32_t>{0x00102000u},
              "retain JALR target when all CFG paths agree");
    }

    // Branch-likely delay-slot writes are visible only on the taken edge.
    // The not-taken edge annuls the delay slot and reaches the same JALR with
    // the pre-branch constant, so the merged target must remain unknown.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1600, encode_lui(25, 0x0010));
        add(insns, 0x1604, encode_ori(25, 25, 0x3000)); // A = 0x00103000
        add(insns, 0x1608, encode_beql(1, 2, 5));      // taken target 0x1620
        add(insns, 0x160c, encode_ori(0, 25, 0x4000)); // delay slot writes B
        add(insns, 0x1610, encode_beq(0, 0, 3));       // branch target 0x1620
        add(insns, 0x1614, 0);                        // ordinary branch delay slot
        add(insns, 0x1618, encode_jr(31));             // alternate return path
        add(insns, 0x161c, 0);
        add(insns, 0x1620, encode_jalr(25));
        add(insns, 0x1624, 0);
        const auto targets = recover(insns, 0x1600);
        check(targets.empty(),
              "branch-likely annulment keeps disagreeing JALR paths unresolved");
    }

    // The audit captures the source register and target value at the
    // JALR instruction itself, before the link write and delay slot.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1700, encode_lui(25, 0x0012));
        add(insns, 0x1704, encode_ori(25, 25, 0x3454));
        add(insns, 0x1708, encode_jalr(25));
        add(insns, 0x170c, encode_ori(0, 25, 0x4000));
        const auto sites = audit_jalr_sites(insns, 0x1700);
        check(sites.size() == 1 &&
              sites[0].m_caller_entry == 0x1700 &&
              sites[0].m_pc == 0x1708 &&
              sites[0].m_source_register == 25 &&
              sites[0].m_source_known &&
              sites[0].m_source_value == 0x00123454u &&
              sites[0].m_target == 0x00123454u &&
              sites[0].m_target_aligned,
              "audit known JALR value before the delay slot");
    }

    // Unknown loads must be reported as unresolved sites, not silently
    // omitted from the coverage report.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1800, encode_lui(25, 0x0010));
        add(insns, 0x1804, encode_lw(8, 25, 0));
        add(insns, 0x1808, encode_jalr(25));
        add(insns, 0x180c, 0);
        const auto sites = audit_jalr_sites(insns, 0x1800);
        check(sites.size() == 1 &&
              !sites[0].m_source_known &&
              !sites[0].m_target_aligned,
              "audit JALR with a load-derived unknown target");
    }

    // A known but non-aligned value is distinct from a dynamic source.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1900, encode_ori(0, 25, 2));
        add(insns, 0x1904, encode_jalr(25));
        add(insns, 0x1908, 0);
        const auto sites = audit_jalr_sites(insns, 0x1900);
        check(sites.size() == 1 &&
              sites[0].m_source_known &&
              sites[0].m_source_value == 2 &&
              sites[0].m_target == 2 &&
              !sites[0].m_target_aligned,
              "audit known but misaligned JALR target");
    }

    // CFG merges only produce a static site value when all incoming paths
    // agree, preserving the existing conservative recovery contract.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1a00, encode_beq(1, 2, 5));
        add(insns, 0x1a04, 0);
        add(insns, 0x1a08, encode_lui(25, 0x0010));
        add(insns, 0x1a0c, encode_ori(25, 25, 0x1000));
        add(insns, 0x1a10, encode_beq(0, 0, 3));
        add(insns, 0x1a14, 0);
        add(insns, 0x1a18, encode_lui(25, 0x0010));
        add(insns, 0x1a1c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1a20, encode_jalr(25));
        add(insns, 0x1a24, 0);
        const auto sites = audit_jalr_sites(insns, 0x1a00);
        check(sites.size() == 1 &&
              !sites[0].m_source_known &&
              !sites[0].m_target_aligned,
              "audit conflicting predecessor values as unresolved");
    }

    std::printf("function_discovery_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
