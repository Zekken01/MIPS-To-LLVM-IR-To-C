// Adapted from ps2-web-recomp:SDBZ by Samir Nuri (2026), MIT license.
// Upstream license: src/runtime/ps2_webgpu/gs/LICENSE.upstream
#pragma once

#include "ee_gif.h"
#include "ee_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// VIF1 state and packet processing adapted from ps2-web-recomp:SDBZ,
// runtime/src/vif.cpp (MIT license, copyright 2026 Samir Nuri).
// LLVM executes the EE guest; VIF1 remains a host-owned PS2 device.
// Unsupported VU execution or VIF stalls never constitute DMA completion.
class c_ee_vif1
{
public:
    static constexpr uint32_t k_register_base = 0x10003c00u;
    static constexpr uint32_t k_fifo_base = 0x10005000u;
    static constexpr uint32_t k_vu1_code_base = 0x11008000u;
    static constexpr uint32_t k_vu1_data_base = 0x1100c000u;
    static constexpr size_t k_vu1_memory_size = 16384u;

    enum class e_reject : uint32_t
    {
        none,
        invalid_packet,
        unsupported_command,
        unsupported_vu_execution,
        irq_requires_resume,
        missing_path2,
        path2_rejected,
        pending_limit
    };

    struct s_diagnostic
    {
        e_reject m_reason = e_reject::none;
        uint32_t m_code = 0u;
        uint32_t m_pending_words = 0u;
        uint32_t m_commands = 0u;
        uint32_t m_mpg_uploads = 0u;
        uint32_t m_unpack_commands = 0u;
        uint32_t m_direct_qwc = 0u;
    };

    void bind_path2(c_ee_gif_path3* path2);
    void reset();
    bool consume(const uint8_t* data, uint32_t qwc);
    const s_diagnostic& diagnostic() const;

    uint32_t read_register(uint32_t offset) const;
    bool write_register(uint32_t offset, uint32_t value);
    const uint8_t* vu1_data() const;
    const uint8_t* vu1_code() const;

    static bool dma_transfer(void* user, uint32_t channel,
                             const uint8_t* data, uint32_t qwc);
    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value);
    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value);

private:
    struct s_machine
    {
        uint32_t m_stat = 0u;
        uint32_t m_err = 0u;
        uint32_t m_mark = 0u;
        uint32_t m_cycle = 0u;
        uint32_t m_mode = 0u;
        uint32_t m_num = 0u;
        uint32_t m_mask = 0u;
        uint32_t m_code = 0u;
        uint32_t m_itops = 0u;
        uint32_t m_base = 0u;
        uint32_t m_ofst = 0u;
        uint32_t m_tops = 0u;
        uint32_t m_itop = 0u;
        uint32_t m_top = 0u;
        uint32_t m_row[4]{};
        uint32_t m_col[4]{};
        bool m_dbf = false;
        std::vector<uint32_t> m_pending;
        std::array<uint8_t, k_vu1_memory_size> m_vu1_data{};
        std::array<uint8_t, k_vu1_memory_size> m_vu1_code{};
        uint32_t m_commands = 0u;
        uint32_t m_mpg_uploads = 0u;
        uint32_t m_unpack_commands = 0u;
        uint32_t m_direct_qwc = 0u;
    };

    static uint32_t command_data_words(const s_machine& machine,
                                       uint32_t code);
    static void unpack(s_machine& machine, uint32_t code,
                       const uint32_t* words);
    static bool execute(s_machine& machine, uint32_t code,
                        const uint32_t* words,
                        std::vector<uint8_t>& path2_bytes,
                        e_reject* reject);

    s_machine m_machine{};
    s_diagnostic m_diagnostic{};
    c_ee_gif_path3* m_path2 = nullptr;
};
} // namespace ee
