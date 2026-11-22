#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace iop
{
class c_memory_card
{
public:
    bool mount(const std::filesystem::path& path);
    bool mounted() const { return !m_data.empty(); }
    uint64_t completed_exchanges() const { return m_completed_exchanges; }
    void reset_protocol();
    bool complete_auth_reset();
    bool exchange(const uint8_t* input, size_t size,
                  std::vector<uint8_t>& reply);
    bool save();

private:
    static constexpr size_t k_page_bytes = 528u;
    static constexpr size_t k_pages = 0x4000u;
    static constexpr size_t k_block_pages = 16u;
    static constexpr size_t k_image_bytes = k_page_bytes * k_pages;

    std::filesystem::path m_path;
    std::vector<uint8_t> m_data;
    size_t m_address = 0u;
    uint64_t m_completed_exchanges = 0u;
    uint8_t m_terminator = 0x55u;
    bool m_dirty = false;
    bool m_auth_reset_pending = false;
};
}
