#include "elf_loader.h"
#include <cstring>
#include <fstream>

namespace ee {

namespace
{
// ELF32 types.
using Elf32_Addr  = uint32_t;
using Elf32_Off   = uint32_t;
using Elf32_Half  = uint16_t;
using Elf32_Word  = uint32_t;
using Elf32_Sword = int32_t;

#pragma pack(push, 1)
struct Elf32_Ehdr
{
    uint8_t  e_ident[16];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff;
    Elf32_Off  e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
};

struct Elf32_Phdr
{
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

struct Elf32_Shdr
{
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
};

struct Elf32_Sym
{
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    Elf32_Half st_shndx;
};
#pragma pack(pop)

constexpr uint32_t ELF_MAGIC = 0x464c457f; // "\x7fELF" little-endian
constexpr uint8_t  ELFCLASS32 = 1;
constexpr uint8_t  ET_EXEC = 2;
constexpr uint8_t  PT_LOAD = 1;
constexpr uint8_t  PF_R = 4, PF_W = 2, PF_X = 1;
constexpr uint8_t  STT_FUNC = 2;
constexpr uint8_t  STT_OBJECT = 1;
constexpr uint8_t  SHN_UNDEF = 0;

inline bool bind_local(uint8_t info) { return (info >> 4) == 0; }
inline uint8_t sym_type(uint8_t info) { return info & 0x0f; }
} // namespace

bool elf_loader::load_file(const char* path, std::string* error)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        if (error) *error = "cannot open file";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 0) {
        std::fclose(f);
        if (error) *error = "cannot determine file size";
        return false;
    }
    m_file.resize(static_cast<size_t>(len));
    if (std::fread(m_file.data(), 1, m_file.size(), f) != m_file.size()) {
        std::fclose(f);
        if (error) *error = "short read";
        return false;
    }
    std::fclose(f);
    return load_buffer(m_file.data(), m_file.size(), error);
}

bool elf_loader::load_buffer(const uint8_t* data, size_t size, std::string* error)
{
    m_file.assign(data, data + size);
    if (!parse_header(data, size)) {
        if (error) *error = "not a valid ELF32 file";
        return false;
    }
    if (!parse_program_headers(data, size)) {
        if (error) *error = "failed to parse program headers";
        return false;
    }
    parse_sections(data, size);
    parse_symbols(data, size);
    return true;
}

bool elf_loader::parse_header(const uint8_t* data, size_t size)
{
    if (size < sizeof(Elf32_Ehdr)) return false;
    const Elf32_Ehdr* hdr = reinterpret_cast<const Elf32_Ehdr*>(data);
    uint32_t magic = 0;
    std::memcpy(&magic, hdr->e_ident, 4);
    if (magic != ELF_MAGIC) return false;
    if (hdr->e_ident[4] != ELFCLASS32) return false;
    if (hdr->e_type != ET_EXEC && hdr->e_type != 1) return false; // relocatable ok too
    if (hdr->e_ehsize < sizeof(Elf32_Ehdr)) return false;
    if (hdr->e_phentsize < sizeof(Elf32_Phdr)) return false;
    if (hdr->e_phoff + (size_t)hdr->e_phnum * hdr->e_phentsize > size) return false;

    m_entry = hdr->e_entry;
    m_machine = hdr->e_machine;
    return true;
}

bool elf_loader::parse_program_headers(const uint8_t* data, size_t size)
{
    // Re-derive header fields locally (parse_header already validated them).
    const Elf32_Ehdr* hdr = reinterpret_cast<const Elf32_Ehdr*>(data);
    for (Elf32_Half i = 0; i < hdr->e_phnum; ++i) {
        const Elf32_Phdr* ph = reinterpret_cast<const Elf32_Phdr*>(
            data + hdr->e_phoff + i * hdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset + ph->p_filesz > size) continue;

        load_segment seg;
        seg.vaddr   = ph->p_vaddr;
        seg.paddr   = ph->p_paddr;
        seg.filesz  = ph->p_filesz;
        seg.memsz   = ph->p_memsz;
        seg.offset  = ph->p_offset;
        seg.align   = ph->p_align ? ph->p_align : 1;
        seg.executable = (ph->p_flags & PF_X) != 0;
        seg.writable   = (ph->p_flags & PF_W) != 0;
        seg.readable   = (ph->p_flags & PF_R) != 0;
        seg.data.assign(data + ph->p_offset,
                        data + ph->p_offset + ph->p_filesz);
        m_segments.push_back(std::move(seg));
    }
    return true;
}

bool elf_loader::parse_sections(const uint8_t* data, size_t size)
{
    const Elf32_Ehdr* hdr = reinterpret_cast<const Elf32_Ehdr*>(data);
    if (hdr->e_shoff == 0 || hdr->e_shnum == 0) return true;
    if (hdr->e_shoff + (size_t)hdr->e_shnum * hdr->e_shentsize > size) return true;
    if (hdr->e_shstrndx == SHN_UNDEF) return true;

    // Section headers.
    const Elf32_Shdr* shdrs = reinterpret_cast<const Elf32_Shdr*>(
        data + hdr->e_shoff);
    // Section name string table.
    const Elf32_Shdr& shstr = shdrs[hdr->e_shstrndx];
    const char* shstrtab = reinterpret_cast<const char*>(data + shstr.sh_offset);

    // Just validate; sections are not otherwise needed for the loader.
    (void)shdrs; (void)shstrtab; (void)size;
    return true;
}

bool elf_loader::parse_symbols(const uint8_t* data, size_t size)
{
    const Elf32_Ehdr* hdr = reinterpret_cast<const Elf32_Ehdr*>(data);
    if (hdr->e_shoff == 0 || hdr->e_shnum == 0) return true;

    const Elf32_Shdr* shdrs = reinterpret_cast<const Elf32_Shdr*>(
        data + hdr->e_shoff);
    const Elf32_Shdr& shstr = shdrs[hdr->e_shstrndx];
    const char* shstrtab = reinterpret_cast<const char*>(data + shstr.sh_offset);

    for (Elf32_Half i = 0; i < hdr->e_shnum; ++i) {
        const Elf32_Shdr& sec = shdrs[i];
        if (sec.sh_type != 2 && sec.sh_type != 11) continue; // SHT_SYMTAB, SHT_DYNSYM
        if (sec.sh_entsize == 0 || sec.sh_size == 0) continue;
        if (sec.sh_link >= hdr->e_shnum) continue;
        const Elf32_Shdr& strsec = shdrs[sec.sh_link];
        const char* strtab = reinterpret_cast<const char*>(data + strsec.sh_offset);

        size_t count = sec.sh_size / sec.sh_entsize;
        const Elf32_Sym* syms = reinterpret_cast<const Elf32_Sym*>(
            data + sec.sh_offset);
        for (size_t j = 0; j < count; ++j) {
            const Elf32_Sym& s = syms[j];
            symbol sym;
            if (s.st_name != 0 && s.st_name < strsec.sh_size) {
                sym.name = strtab + s.st_name;
            }
            sym.value = s.st_value;
            sym.size = s.st_size;
            sym.is_function = sym_type(s.st_info) == STT_FUNC;
            sym.is_undefined = s.st_shndx == SHN_UNDEF;
            m_symbols.push_back(std::move(sym));
        }
    }
    return true;
}

size_t elf_loader::read_at(uint32_t addr, uint8_t* out, size_t max_len) const
{
    size_t total = 0;
    for (const auto& seg : m_segments) {
        if (addr < seg.vaddr) continue;
        uint32_t off = addr - seg.vaddr;
        if (off >= seg.filesz) continue;
        size_t avail = seg.filesz - off;
        size_t take = std::min(avail, max_len - total);
        std::memcpy(out + total, seg.data.data() + off, take);
        total += take;
        if (total >= max_len) break;
    }
    return total;
}

bool elf_loader::read32_at(uint32_t addr, uint32_t* out) const
{
    uint8_t buf[4];
    if (read_at(addr, buf, 4) != 4) return false;
    *out = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return true;
}

bool elf_loader::is_executable_address(uint32_t addr) const
{
    for (const auto& seg : m_segments) {
        if (addr >= seg.vaddr && addr < seg.vaddr + seg.memsz &&
            seg.executable) {
            return true;
        }
    }
    return false;
}

const elf_loader::symbol* elf_loader::find_symbol(uint32_t addr) const
{
    for (const auto& s : m_symbols) {
        if (!s.is_undefined && s.value == addr) return &s;
    }
    return nullptr;
}

const elf_loader::symbol* elf_loader::find_function_at(uint32_t addr) const
{
    const symbol* best = nullptr;
    for (const auto& s : m_symbols) {
        if (s.is_function && !s.is_undefined && s.value <= addr) {
            if (!best || s.value > best->value) best = &s;
        }
    }
    return best;
}

} // namespace ee
