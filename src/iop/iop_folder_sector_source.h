#pragma once

#include "iop_cdvd.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace iop
{
struct s_folder_file_info
{
    uint32_t m_lba = 0u;
    uint32_t m_size = 0u;
};

// An in-memory ISO9660 directory and sector map over an extracted disc.
// GAME/ and NATIVE/ are host-only native assets, not original disc entries.
// This source NEVER creates INFO.DAT or GAME.DAT and requires no sidecars.
class c_folder_sector_source final : public c_sector_source
{
public:
    explicit c_folder_sector_source(const std::filesystem::path& root);

    bool valid() const;
    const std::string& error() const;
    uint64_t sector_count() const override;
    bool read_sectors(
        uint32_t lba, uint32_t count, std::vector<uint8_t>& data
    ) override;

    // Lookup semantics for a future HLE sceCdSearchFile adapter.
    // Only files are returned; traversal and unknown paths fail closed.
    bool find_file(
        const std::string& guest_path, s_folder_file_info* output
    ) const;

private:
    struct s_node
    {
        std::filesystem::path m_host_path;
        std::string m_name;
        std::string m_parent;
        uint32_t m_lba = 0u;
        uint32_t m_size = 0u;
        bool m_directory = false;
    };

    struct s_region
    {
        uint32_t m_count = 0u;
        uint32_t m_size = 0u;
        std::filesystem::path m_host_path;
        std::vector<uint8_t> m_memory;
    };

    bool build(const std::filesystem::path& root);
    bool fail(const std::string& reason);
    void add_memory(uint32_t lba, std::vector<uint8_t> bytes);
    std::vector<uint8_t> make_directory(const std::string& key) const;
    std::vector<uint8_t> make_record(
        const s_node& node, const std::string& name
    ) const;

    std::map<std::string, s_node> m_nodes;
    std::map<uint32_t, s_region> m_regions;
    std::string m_error;
    uint64_t m_sector_count = 0u;
    bool m_valid = false;
};
} // namespace iop
