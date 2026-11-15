#include "ee_gs.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace ee
{
namespace
{
constexpr uint8_t k_block32[4][8] =
{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31}
};

constexpr uint8_t k_column32[2][8] =
{
    {0, 1, 4, 5, 8, 9, 12, 13},
    {2, 3, 6, 7, 10, 11, 14, 15}
};

// Exact PSMCT16 block/column permutations from ps2-web-recomp.
constexpr uint8_t k_block16[8][4] =
{
    {0, 2, 8, 10},
    {1, 3, 9, 11},
    {4, 6, 12, 14},
    {5, 7, 13, 15},
    {16, 18, 24, 26},
    {17, 19, 25, 27},
    {20, 22, 28, 30},
    {21, 23, 29, 31}
};

constexpr uint8_t k_block16s[8][4] =
{
    {0, 2, 16, 18},
    {1, 3, 17, 19},
    {8, 10, 24, 26},
    {9, 11, 25, 27},
    {4, 6, 20, 22},
    {5, 7, 21, 23},
    {12, 14, 28, 30},
    {13, 15, 29, 31}
};

constexpr uint8_t k_column16[2][16] =
{
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
    {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}
};

// PSMT8 uses the reference's word-column permutation; each index
// selects a single byte lane within the corresponding 32-bit word.
constexpr uint8_t k_column_word[2][2][8] =
{
    {
        {0, 1, 4, 5, 8, 9, 12, 13},
        {2, 3, 6, 7, 10, 11, 14, 15}
    },
    {
        {8, 9, 12, 13, 0, 1, 4, 5},
        {10, 11, 14, 15, 2, 3, 6, 7}
    }
};

uint32_t read_le32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8u) |
        (static_cast<uint32_t>(data[2]) << 16u) |
        (static_cast<uint32_t>(data[3]) << 24u);
}
} // namespace

uint32_t c_ee_gs_state::psmct32_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // The reference maps PSMCT32 at 64x32 pixels per 8192-byte page.
    const uint64_t page =
        (static_cast<uint64_t>(x / 64u) +
         static_cast<uint64_t>(y / 32u) * width) * 8192u;
    const uint32_t block = k_block32[(y & 31u) / 8u][(x & 63u) / 8u];
    const uint32_t column = ((y & 7u) / 2u) * 64u +
        static_cast<uint32_t>(k_column32[y & 1u][x & 7u]) * 4u;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page +
         block * 256u + column) & (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmct16_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y,
    uint32_t psm)
{
    // 64x64 pixels per 8192-byte page, DBW in 64-pixel units.
    const uint64_t page =
        (static_cast<uint64_t>(x / 64u) +
         static_cast<uint64_t>(y / 64u) * width) * 8192u;
    const uint32_t block = psm == 0x0au ?
        k_block16s[(y & 63u) / 8u][(x & 63u) / 16u] :
        k_block16[(y & 63u) / 8u][(x & 63u) / 16u];
    const uint32_t column = ((y & 7u) / 2u) * 64u +
        static_cast<uint32_t>(k_column16[y & 1u][x & 15u]) * 2u;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page +
         block * 256u + column) & (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmt8_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // Same 128x64 page, block, word and byte layout as the reference's
    // build_tables() / page_address() / write_pixel(PSMT8) path.
    // Its page stride truncates DBW*64/128, with a minimum of one page.
    const uint32_t pages_per_row = std::max(1u, width / 2u);
    const uint64_t page =
        (static_cast<uint64_t>(x / 128u) +
         static_cast<uint64_t>(y / 64u) * pages_per_row) * 8192u;

    const uint32_t block =
        k_block32[(y & 63u) / 16u][(x & 127u) / 16u];
    const uint32_t wx = x & 15u;
    const uint32_t inner_y = y & 15u;
    const uint32_t column = inner_y / 4u;
    const uint32_t wy = inner_y & 3u;
    const uint32_t table =
        ((wy & 2u) >> 1u) ^ ((y / 4u) & 1u);
    const uint32_t lane =
        ((wx & 8u) >> 2u) + ((wy & 2u) >> 1u);
    const uint32_t in_page =
        block * 256u + column * 64u +
        static_cast<uint32_t>(
            k_column_word[table][wy & 1u][wx & 7u]) * 4u + lane;

    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page + in_page) &
        (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmt4_nibble_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // PSMT4 page_address(bp,bw,x,y,128,128) + g_off4[y&127][x&127].
    // The reference uses the normal PSMCT16 block table, NOT 16S.
    const uint32_t pages_per_row = std::max(1u, width / 2u);
    const uint64_t page =
        (static_cast<uint64_t>(x / 128u) +
         static_cast<uint64_t>(y / 128u) * pages_per_row) * 8192u;
    const uint32_t block =
        k_block16[(y & 127u) / 16u][(x & 127u) / 32u];

    const uint32_t wx = x & 31u;
    const uint32_t local_y = y & 15u;
    const uint32_t column = local_y / 4u;
    const uint32_t wy = local_y & 3u;
    const uint32_t nibble =
        ((wx & 0x18u) + ((wy & 2u) << 1u)) / 4u;
    const uint32_t table =
        ((wy & 2u) >> 1u) ^ (column & 1u);
    const uint32_t inner =
        column * 64u + block * 256u +
        static_cast<uint32_t>(
            k_column_word[table][wy & 1u][wx & 7u]) * 4u;

    const uint64_t address =
        (static_cast<uint64_t>(base) * 256u + page + inner) * 2u +
        nibble;
    return static_cast<uint32_t>(
        address & (2u * k_vram_bytes - 1u)
    );
}

const std::vector<uint8_t>& c_ee_gs_state::vram() const
{
    return m_vram;
}

bool c_ee_gs_state::read_vram8(uint32_t offset, uint8_t& value) const
{
    if (offset >= k_vram_bytes)
    {
        return false;
    }
    value = m_vram[offset];
    return true;
}

bool c_ee_gs_state::read_vram16(uint32_t offset, uint16_t& value) const
{
    if (offset > k_vram_bytes - 2u)
    {
        return false;
    }
    value = static_cast<uint16_t>(
        static_cast<uint16_t>(m_vram[offset]) |
        static_cast<uint16_t>(m_vram[offset + 1u] << 8u)
    );
    return true;
}

bool c_ee_gs_state::read_vram32(uint32_t offset, uint32_t& value) const
{
    if (offset > k_vram_bytes - 4u)
    {
        return false;
    }
    value = read_le32(m_vram.data() + offset);
    return true;
}

bool c_ee_gs_state::read_clut_entry(
    uint32_t slot, uint32_t& value) const
{
    if (slot >= m_state.m_clut.size() ||
        !m_state.m_clut_valid[slot])
    {
        return false;
    }
    value = m_state.m_clut[slot];
    return true;
}

bool c_ee_gs_state::read_indexed_clut_value(
    uint32_t context, uint32_t x, uint32_t y,
    uint32_t& value) const
{
    if (context >= 2u || !m_state.m_written[0x06u + context])
    {
        return false;
    }

    const uint64_t tex0 = m_state.m_registers[0x06u + context];
    const uint32_t bp = static_cast<uint32_t>(tex0 & 0x3fffu);
    const uint32_t bw = static_cast<uint32_t>((tex0 >> 14u) & 0x3fu);
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20u) & 0x3fu);
    const uint32_t csa = static_cast<uint32_t>((tex0 >> 56u) & 0x1fu);
    uint32_t slot = 0u;

    if (psm == 0x13u) // PSMT8
    {
        slot = m_vram[psmt8_address(bp, bw, x, y)];
    }
    else if (psm == 0x14u) // PSMT4
    {
        const uint32_t nibble = psmt4_nibble_address(bp, bw, x, y);
        const uint32_t index = (
            m_vram[nibble / 2u] >> ((nibble & 1u) * 4u)
        ) & 0x0fu;
        // Same CLUT slot selection as the reference's dump_texture().
        slot = (csa * 16u + index) & 511u;
    }
    else
    {
        return false;
    }

    return read_clut_entry(slot, value);
}

void c_ee_gs_state::reset()
{
    m_state = {};
    std::fill(m_vram.begin(), m_vram.end(), 0u);
}

const s_ee_gs_state& c_ee_gs_state::state() const
{
    return m_state;
}

bool c_ee_gs_state::was_written(uint32_t reg) const
{
    return reg < m_state.m_written.size() && m_state.m_written[reg];
}

uint64_t c_ee_gs_state::register_value(uint32_t reg) const
{
    return reg < m_state.m_registers.size() ?
        m_state.m_registers[reg] : 0u;
}

bool c_ee_gs_state::load_clut(
    s_ee_gs_state& state, uint64_t tex0,
    const std::vector<s_vram_write>& writes) const
{
    const uint32_t cld = static_cast<uint32_t>((tex0 >> 61u) & 7u);
    if (cld == 0u || cld >= 6u)
    {
        return true;
    }

    const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37u) & 0x3fffu);
    if ((cld == 4u && state.m_clut_cbp0 == cbp) ||
        (cld == 5u && state.m_clut_cbp1 == cbp))
    {
        return true;
    }

    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20u) & 0x3fu);
    const uint32_t cpsm = static_cast<uint32_t>((tex0 >> 51u) & 0x0fu);
    const uint32_t csm = static_cast<uint32_t>((tex0 >> 55u) & 1u);
    const uint32_t csa = static_cast<uint32_t>((tex0 >> 56u) & 0x1fu);
    // A CLD-triggered load of a different indexed format or unknown
    // palette storage layout cannot yet be executed faithfully.
    if ((psm != 0x13u && psm != 0x14u) ||
        (cpsm != 0u && cpsm != 2u && cpsm != 0x0au))
    {
        return false;
    }

    const bool eight = psm == 0x13u;
    const uint32_t entries = eight ? 256u : 16u;
    const uint32_t entry_bytes = cpsm == 0u ? 4u : 2u;
    const uint32_t texclut = static_cast<uint32_t>(
        state.m_registers[0x1cu]
    );
    const uint32_t cbw = csm ? (texclut & 0x3fu) : 1u;
    const uint32_t cou = (texclut >> 6u) & 0x3fu;
    const uint32_t cov = (texclut >> 12u) & 0x3ffu;

    std::array<uint32_t, 256> addresses{};
    // Collect at most 1024 palette bytes. An optional same-batch IMAGE
    // is overlaid only at these bytes, not copied into a 4 MiB VRAM
    // snapshot or scanned separately for every palette entry.
    std::unordered_map<uint32_t, uint8_t> palette_bytes;
    palette_bytes.reserve(entries * entry_bytes);

    for (uint32_t i = 0u; i < entries; ++i)
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
        if (csm != 0u) // CSM2: TEXCLUT.CBW/COU/COV
        {
            x = cou * 16u + i;
            y = cov;
        }
        else if (eight) // CSM1: swap index bits 3 and 4
        {
            const uint32_t j = (i & ~0x18u) |
                ((i & 0x08u) << 1u) | ((i & 0x10u) >> 1u);
            x = j & 15u;
            y = j >> 4u;
        }
        else // CSM1: 4-bit entries occupy an 8x2 rectangle
        {
            x = i & 7u;
            y = i >> 3u;
        }

        const uint32_t address = cpsm == 0u ?
            psmct32_address(cbp, std::max(1u, cbw), x, y) :
            psmct16_address(cbp, std::max(1u, cbw), x, y, cpsm);
        addresses[i] = address;
        for (uint32_t b = 0u; b < entry_bytes; ++b)
        {
            const uint32_t at = (address + b) & (k_vram_bytes - 1u);
            palette_bytes.emplace(at, m_vram[at]);
        }
    }

    // Model exactly the bytes visible at this TEX0/TEX2 instruction.
    // This also handles multiple writes to a single nibble in order.
    for (const s_vram_write& write : writes)
    {
        if (write.m_bytes == 0u)
        {
            auto it = palette_bytes.find(write.m_offset);
            if (it != palette_bytes.end())
            {
                const uint8_t mask = static_cast<uint8_t>(
                    0x0fu << write.m_shift
                );
                it->second = static_cast<uint8_t>(
                    (it->second & static_cast<uint8_t>(~mask)) |
                    static_cast<uint8_t>(
                        (write.m_value & 0x0fu) << write.m_shift
                    )
                );
            }
        }
        else
        {
            for (uint32_t b = 0u; b < write.m_bytes; ++b)
            {
                const uint32_t at =
                    (write.m_offset + b) & (k_vram_bytes - 1u);
                auto it = palette_bytes.find(at);
                if (it != palette_bytes.end())
                {
                    it->second = static_cast<uint8_t>(
                        write.m_value >> (8u * b)
                    );
                }
            }
        }
    }

    for (uint32_t i = 0u; i < entries; ++i)
    {
        uint32_t value = 0u;
        for (uint32_t b = 0u; b < entry_bytes; ++b)
        {
            const uint32_t at =
                (addresses[i] + b) & (k_vram_bytes - 1u);
            value |= static_cast<uint32_t>(palette_bytes.at(at))
                << (8u * b);
        }

        const uint32_t slot = csm != 0u ?
            (csa * 16u + i) & 511u :
            eight ? i : ((csa & 15u) * 16u + i) & 255u;
        state.m_clut[slot] = value;
        state.m_clut_valid[slot] = true;
    }

    if (cld == 2u || cld == 4u)
    {
        state.m_clut_cbp0 = cbp;
    }
    else if (cld == 3u || cld == 5u)
    {
        state.m_clut_cbp1 = cbp;
    }
    ++state.m_clut_load_count;
    return true;
}

bool c_ee_gs_state::write_register(
    s_ee_gs_state& state, uint32_t reg, uint64_t value)
{
    // These registers have hardware side effects that a state-only
    // observer must NOT silently pretend to execute.
    if (reg >= state.m_registers.size())
    {
        return false;
    }

    switch (reg)
    {
    case 0x00u: // PRIM
        state.m_vertex_count = 0u;
        break;
    case 0x04u: // XYZF2
    case 0x05u: // XYZ2
    case 0x0cu: // XYZF3 (ADC)
    case 0x0du: // XYZ3 (ADC)
        ++state.m_vertex_count;
        break;
    case 0x01u: // RGBAQ
    case 0x02u: // ST
    case 0x03u: // UV
    case 0x06u: // TEX0_1
    case 0x07u: // TEX0_2
    case 0x08u: // CLAMP_1
    case 0x09u: // CLAMP_2
    case 0x0au: // FOG
    case 0x14u: // TEX1_1
    case 0x15u: // TEX1_2
    case 0x18u: // XYOFFSET_1
    case 0x19u: // XYOFFSET_2
    case 0x1au: // PRMODECONT
    case 0x1bu: // PRMODE
    case 0x1cu: // TEXCLUT
    case 0x22u: // SCANMSK
    case 0x34u: // MIPTBP1_1
    case 0x35u: // MIPTBP1_2
    case 0x36u: // MIPTBP2_1
    case 0x37u: // MIPTBP2_2
    case 0x3bu: // TEXA
    case 0x3du: // FOGCOL
    case 0x40u: // SCISSOR_1
    case 0x41u: // SCISSOR_2
    case 0x42u: // ALPHA_1
    case 0x43u: // ALPHA_2
    case 0x44u: // DIMX
    case 0x45u: // DTHE
    case 0x46u: // COLCLAMP
    case 0x47u: // TEST_1
    case 0x48u: // TEST_2
    case 0x49u: // PABE
    case 0x4au: // FBA_1
    case 0x4bu: // FBA_2
    case 0x4cu: // FRAME_1
    case 0x4du: // FRAME_2
    case 0x4eu: // ZBUF_1
    case 0x4fu: // ZBUF_2
    case 0x50u: // BITBLTBUF
    case 0x51u: // TRXPOS
    case 0x52u: // TRXREG
        break;
    case 0x16u: // TEX2_1 modifies selected fields of TEX0_1
    case 0x17u: // TEX2_2 modifies selected fields of TEX0_2
    {
        // Exact bit mask from ps2-web-recomp/runtime/src/gs/gs.cpp.
        constexpr uint64_t k_tex2_mask = 0xffffffe003f00000ull;
        const uint32_t tex0 = 0x06u + (reg - 0x16u);
        state.m_registers[tex0] =
            (state.m_registers[tex0] & ~k_tex2_mask) |
            (value & k_tex2_mask);
        state.m_written[tex0] = true;
        break;
    }
    case 0x53u: // TRXDIR
        state.m_transfer_started = false;
        state.m_transfer_pixels_written = 0u;
        state.m_transfer_pending_size = 0u;
        if ((value & 3u) == 0u)
        {
            // Only verified PSMCT32/24/16/16S and PSMT8/PSMT4 uploads execute.
            // Unsupported formats and malformed setup reject the batch.
            if (!state.m_written[0x50u] ||
                !state.m_written[0x51u] ||
                !state.m_written[0x52u])
            {
                return false;
            }
            const uint64_t buf = state.m_registers[0x50u];
            const uint64_t reg_size = state.m_registers[0x52u];
            const uint32_t dest_width =
                static_cast<uint32_t>((buf >> 48u) & 0x3fu);
            const uint32_t dest_psm =
                static_cast<uint32_t>((buf >> 56u) & 0x3fu);
            const uint32_t pixel_width =
                static_cast<uint32_t>(reg_size & 0xfffu);
            const uint32_t pixel_height =
                static_cast<uint32_t>((reg_size >> 32u) & 0xfffu);
            if (dest_width == 0u ||
                (dest_psm != 0u && dest_psm != 1u &&
                 dest_psm != 2u && dest_psm != 0x0au &&
                 dest_psm != 0x13u && dest_psm != 0x14u) ||
                pixel_width == 0u || pixel_height == 0u)
            {
                return false;
            }
            state.m_image_data.clear();
            state.m_image_qwc = 0u;
            state.m_transfer_started = true;
        }
        break;
    case 0x0eu: // GS ignores direct writes to these unmapped registers
    case 0x0fu:
        return true;
    default:
        // Especially reject HWREG, SIGNAL, FINISH, LABEL, TEXFLUSH
        // and unknown registers; they need real GS behavior.
        return false;
    }

    state.m_registers[reg] = value;
    state.m_written[reg] = true;
    return true;
}

bool c_ee_gs_state::write_packed(
    s_ee_gs_state& state, uint32_t reg, uint64_t low, uint64_t high)
{
    switch (reg)
    {
    case 0x00u: // PRIM
        return write_register(state, 0x00u, low & 0x7ffu);
    case 0x01u: // RGBAQ (four widened bytes plus current Q bits)
    {
        const uint64_t rgba =
            (low & 0xffu) |
            (((low >> 32u) & 0xffu) << 8u) |
            ((high & 0xffu) << 16u) |
            (((high >> 32u) & 0xffu) << 24u) |
            (static_cast<uint64_t>(state.m_q_bits) << 32u);
        return write_register(state, 0x01u, rgba);
    }
    case 0x02u: // STQ
    {
        if (!write_register(state, 0x02u, low))
        {
            return false;
        }
        state.m_q_bits = static_cast<uint32_t>(high);
        return true;
    }
    case 0x03u: // UV (two masked 14-bit coordinates)
        return write_register(
            state, 0x03u,
            (low & 0x3fffu) |
            (((low >> 32u) & 0x3fffu) << 16u)
        );
    case 0x04u: // XYZF2, ADC bit selects XYZF3
    case 0x0cu: // XYZF3
    {
        const uint64_t xyzf =
            (low & 0xffffu) |
            (((low >> 32u) & 0xffffu) << 16u) |
            (((high >> 4u) & 0xffffffu) << 32u) |
            (((high >> 36u) & 0xffu) << 56u);
        const uint32_t dest = reg == 0x0cu ? 0x0cu :
            ((high >> 47u) & 1u) != 0u ? 0x0cu : 0x04u;
        return write_register(state, dest, xyzf);
    }
    case 0x05u: // XYZ2, ADC bit selects XYZ3
    case 0x0du: // XYZ3
    {
        const uint64_t xyz =
            (low & 0xffffu) |
            (((low >> 32u) & 0xffffu) << 16u) |
            ((high & 0xffffffffu) << 32u);
        const uint32_t dest = reg == 0x0du ? 0x0du :
            ((high >> 47u) & 1u) != 0u ? 0x0du : 0x05u;
        return write_register(state, dest, xyz);
    }
    case 0x0au: // FOG from W3 bits 4..11
        return write_register(
            state, 0x0au, ((high >> 36u) & 0xffu) << 56u
        );
    case 0x0eu: // A+D GS register address in low byte of high word
        return write_register(
            state, static_cast<uint32_t>(high & 0xffu), low
        );
    case 0x0fu: // NOP
        return true;
    default:
        // The remaining packed registers map directly to their GS
        // addresses in the ps2-web-recomp GS packed writer.
        return write_register(state, reg, low);
    }
}

bool c_ee_gs_state::consume(
    const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    if ((count != 0u && events == nullptr) ||
        (qwc != 0u && data == nullptr) ||
        qwc > 0xffffu)
    {
        return false;
    }

    // A local snapshot makes every accepted DMA event batch atomic.
    s_ee_gs_state next = m_state;
    std::vector<s_vram_write> writes;
    const uint64_t bytes = static_cast<uint64_t>(qwc) * 16u;

    // TEX0 and TEX2 load CLUT entries at the register write, not at
    // the end of the GIF batch or on every subsequent VRAM write.
    const auto refresh_clut = [&](uint32_t reg) -> bool
    {
        uint32_t tex0_reg = 0u;
        if (reg == 0x06u || reg == 0x07u)
        {
            tex0_reg = reg;
        }
        else if (reg == 0x16u || reg == 0x17u)
        {
            tex0_reg = 0x06u + (reg - 0x16u);
        }
        else
        {
            return true;
        }
        return load_clut(next, next.m_registers[tex0_reg], writes);
    };

    for (size_t i = 0; i < count; ++i)
    {
        const s_ee_gif_event& e = events[i];
        const uint64_t offset = e.m_byte_offset;
        if (offset >= bytes)
        {
            return false;
        }

        switch (e.m_type)
        {
        case e_ee_gif_event_type::tag:
            if ((offset & 15u) != 0u || offset + 16u > bytes)
            {
                return false;
            }
            next.m_q_bits = 0x3f800000u;
            ++next.m_tag_count;
            break;
        case e_ee_gif_event_type::primitive:
            if ((offset & 15u) != 0u ||
                offset + 16u > bytes ||
                e.m_word0 > 0x7ffu ||
                !write_register(next, 0x00u, e.m_word0))
            {
                return false;
            }
            break;
        case e_ee_gif_event_type::packed:
            if ((offset & 15u) != 0u ||
                offset + 16u > bytes ||
                e.m_register > 0xfu ||
                !write_packed(
                    next, e.m_register, e.m_word0, e.m_word1
                ) ||
                !refresh_clut(
                    e.m_register == 0x0eu ?
                    static_cast<uint32_t>(e.m_word1 & 0xffu) :
                    e.m_register
                ))
            {
                return false;
            }
            break;
        case e_ee_gif_event_type::reglist:
            if ((offset & 7u) != 0u ||
                offset + 8u > bytes ||
                e.m_register > 0xfu ||
                !write_register(next, e.m_register, e.m_word0) ||
                !refresh_clut(e.m_register))
            {
                return false;
            }
            break;
        case e_ee_gif_event_type::image:
        {
            const uint64_t length =
                static_cast<uint64_t>(e.m_qwc) * 16u;
            // IMAGE data without a configured host-to-local transfer
            // cannot safely be claimed as received by a GS device.
            if ((offset & 15u) != 0u ||
                e.m_qwc == 0u ||
                offset + length > bytes ||
                !next.m_written[0x50u] ||
                !next.m_written[0x51u] ||
                !next.m_written[0x52u] ||
                !next.m_written[0x53u] ||
                (next.m_registers[0x53u] & 3u) != 0u ||
                !next.m_transfer_started ||
                length > k_max_image_bytes -
                    next.m_image_data.size())
            {
                return false;
            }
            next.m_image_data.insert(
                next.m_image_data.end(),
                data + static_cast<size_t>(offset),
                data + static_cast<size_t>(offset + length)
            );
            next.m_image_qwc += e.m_qwc;

            // PSMCT32/24 share 64x32 pages, PSMCT16/16S use 64x64
            // pages, PSMT8 uses 128x64, and PSMT4 uses 128x128
            // pages with nibble swizzling. Only PSMCT24 needs carry.
            const uint64_t buf = next.m_registers[0x50u];
            const uint64_t pos = next.m_registers[0x51u];
            const uint64_t reg_size = next.m_registers[0x52u];
            const uint32_t base =
                static_cast<uint32_t>((buf >> 32u) & 0x3fffu);
            const uint32_t width =
                static_cast<uint32_t>((buf >> 48u) & 0x3fu);
            const uint32_t psm =
                static_cast<uint32_t>((buf >> 56u) & 0x3fu);
            const uint32_t x =
                static_cast<uint32_t>((pos >> 32u) & 0x7ffu);
            const uint32_t y =
                static_cast<uint32_t>((pos >> 48u) & 0x7ffu);
            const uint32_t rrw = static_cast<uint32_t>(reg_size & 0xfffu);
            const uint32_t rrh =
                static_cast<uint32_t>((reg_size >> 32u) & 0xfffu);
            const uint32_t total_pixels = rrw * rrh;

            // A transfer setup may have been rewritten after TRXDIR;
            // reject it rather than applying a guessed pixel layout.
            if (width == 0u ||
                (psm != 0u && psm != 1u && psm != 2u &&
                 psm != 0x0au && psm != 0x13u &&
                 psm != 0x14u) ||
                rrw == 0u || rrh == 0u ||
                next.m_transfer_pixels_written > total_pixels)
            {
                return false;
            }

            const auto stage_pixel = [&](uint32_t value, uint8_t size)
            {
                const uint32_t pixel = next.m_transfer_pixels_written;
                const uint32_t dx = (x + pixel % rrw) & 0x7ffu;
                const uint32_t dy = (y + pixel / rrw) & 0x7ffu;
                uint32_t address = 0u;
                uint8_t shift = 0u;
                if (psm == 0x14u)
                {
                    const uint32_t nibble_address =
                        psmt4_nibble_address(base, width, dx, dy);
                    address = nibble_address / 2u;
                    shift = (nibble_address & 1u) ? 4u : 0u;
                }
                else if (psm == 0x13u)
                {
                    address = psmt8_address(base, width, dx, dy);
                }
                else if (psm == 2u || psm == 0x0au)
                {
                    address = psmct16_address(base, width, dx, dy, psm);
                }
                else
                {
                    address = psmct32_address(base, width, dx, dy);
                }
                writes.push_back({address, value, size, shift});
                ++next.m_transfer_pixels_written;
            };

            if (psm == 0x14u)
            {
                // Packed GIF pixels: first index is the low nibble.
                // Each 16-byte IMAGE qword carries 32 indices, so
                // no partial nibble is carried between DMA batches.
                if (next.m_transfer_pending_size != 0u)
                {
                    return false;
                }
                const uint32_t remaining =
                    total_pixels - next.m_transfer_pixels_written;
                const uint32_t pixels = static_cast<uint32_t>(
                    std::min<uint64_t>(length * 2u, remaining)
                );
                for (uint32_t j = 0u; j < pixels; ++j)
                {
                    const uint8_t packed =
                        data[static_cast<size_t>(offset + j / 2u)];
                    const uint32_t value =
                        (packed >> ((j & 1u) * 4u)) & 0x0fu;
                    stage_pixel(value, 0u);
                }
            }
            else if (psm == 0u || psm == 2u ||
                     psm == 0x0au || psm == 0x13u)
            {
                // 1-, 2- and 4-byte indices/pixels fit GIF qwords.
                const uint8_t pixel_bytes = psm == 0x13u ? 1u :
                    (psm == 2u || psm == 0x0au) ? 2u : 4u;
                if (next.m_transfer_pending_size != 0u)
                {
                    return false;
                }
                const uint32_t remaining =
                    total_pixels - next.m_transfer_pixels_written;
                const uint32_t pixels = static_cast<uint32_t>(
                    std::min<uint64_t>(length / pixel_bytes, remaining)
                );
                for (uint32_t j = 0u; j < pixels; ++j)
                {
                    const size_t data_offset =
                        static_cast<size_t>(offset) +
                        static_cast<size_t>(j) * pixel_bytes;
                    uint32_t value = data[data_offset];
                    if (pixel_bytes == 2u)
                    {
                        value |= static_cast<uint32_t>(
                            data[data_offset + 1u]) << 8u;
                    }
                    else if (pixel_bytes == 4u)
                    {
                        value = read_le32(data + data_offset);
                    }
                    stage_pixel(value, pixel_bytes);
                }
            }
            else
            {
                for (uint64_t j = 0u;
                     j < length &&
                     next.m_transfer_pixels_written < total_pixels; ++j)
                {
                    next.m_transfer_pending[next.m_transfer_pending_size++] =
                        data[static_cast<size_t>(offset + j)];
                    if (next.m_transfer_pending_size == 3u)
                    {
                        const uint32_t pixel =
                            static_cast<uint32_t>(next.m_transfer_pending[0]) |
                            (static_cast<uint32_t>(next.m_transfer_pending[1]) << 8u) |
                            (static_cast<uint32_t>(next.m_transfer_pending[2]) << 16u);
                        stage_pixel(pixel, 3u);
                        next.m_transfer_pending_size = 0u;
                    }
                }
            }
            // Padding after the final pixel is captured but not written.
            break;
        }
        default:
            return false;
        }
    }

    // Commit only after every event has passed validation. Late failures
    // cannot leave partial VRAM pixels behind even if the GIF DMA rejects.
    for (const s_vram_write& write : writes)
    {
        const uint32_t offset = write.m_offset;
        const uint32_t value = write.m_value;
        if (write.m_bytes == 0u)
        {
            // Apply staged nibble writes in order; multiple indexed
            // pixels can share the same underlying VRAM byte.
            const uint8_t mask =
                static_cast<uint8_t>(0x0fu << write.m_shift);
            m_vram[offset] = static_cast<uint8_t>(
                (m_vram[offset] & static_cast<uint8_t>(~mask)) |
                static_cast<uint8_t>((value & 0x0fu) << write.m_shift)
            );
            continue;
        }
        m_vram[offset + 0u] = static_cast<uint8_t>(value);
        if (write.m_bytes >= 2u)
        {
            m_vram[offset + 1u] = static_cast<uint8_t>(value >> 8u);
        }
        if (write.m_bytes >= 3u)
        {
            m_vram[offset + 2u] = static_cast<uint8_t>(value >> 16u);
        }
        if (write.m_bytes == 4u)
        {
            m_vram[offset + 3u] = static_cast<uint8_t>(value >> 24u);
        }
    }
    m_state = std::move(next);
    return true;
}

bool c_ee_gs_state::gif_events(
    void* user, const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    if (user == nullptr)
    {
        return false;
    }
    return static_cast<c_ee_gs_state*>(user)->consume(
        events, count, data, qwc
    );
}
} // namespace ee
