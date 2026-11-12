#pragma once
#include "ee_state.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace ee {

class elf_loader
{
public:
    struct load_segment
    {
        uint32_t vaddr = 0;
        uint32_t paddr = 0;
        uint32_t filesz = 0;
        uint32_t memsz = 0;
        uint32_t offset = 0;
        uint32_t align = 1;
        bool executable = false;
        bool writable = false;
        bool readable = false;
        std::vector<uint8_t> data;
    };

    struct symbol
    {
        std::string name;
        uint32_t value = 0;
        uint32_t size = 0;
        bool is_function = false;
        bool is_undefined = false;
    };

    elf_loader() = default;
    bool load_file(const char* path, std::string* error = nullptr);
    bool load_buffer(const uint8_t* data, size_t size, std::string* error = nullptr);

    uint32_t entry_point() const { return m_entry; }
    uint16_t machine() const { return m_machine; }
    const std::vector<load_segment>& segments() const { return m_segments; }
    const std::vector<symbol>& symbols() const { return m_symbols; }
    size_t read_at(uint32_t addr, uint8_t* out, size_t max_len) const;
    bool read32_at(uint32_t addr, uint32_t* out) const;
    // Read a file-backed word only from an ET_EXEC PT_LOAD segment marked
    // readable and not writable, with a complete section table proving that
    // no relocation overlaps the word. Used for conservative static pointer dataflow.
    bool read32_const_at(uint32_t addr, uint32_t* out) const;
    bool is_executable_address(uint32_t addr) const;
    const symbol* find_symbol(uint32_t addr) const;
    const symbol* find_function_at(uint32_t addr) const;

private:
    bool parse_header(const uint8_t* data, size_t size);
    bool parse_program_headers(const uint8_t* data, size_t size);
    bool parse_sections(const uint8_t* data, size_t size);
    bool parse_symbols(const uint8_t* data, size_t size);

    uint32_t m_entry = 0;
    uint16_t m_machine = 0;
    uint16_t m_type = 0;
    bool m_relocations_complete = false;
    bool m_has_dynamic_segment = false;
    std::set<uint32_t> m_relocation_addresses;
    std::vector<load_segment> m_segments;
    std::vector<symbol> m_symbols;
    std::vector<uint8_t> m_file;
};

} // namespace ee
