#include "ee_gs.h"

namespace ee
{
void c_ee_gs_state::reset()
{
    m_state = {};
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
        if ((value & 3u) == 0u)
        {
            // Start a new host-to-local transfer capture. This stores
            // raw IMAGE bytes but does not touch GS VRAM.
            state.m_image_data.clear();
            state.m_image_qwc = 0u;
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
    const uint64_t bytes = static_cast<uint64_t>(qwc) * 16u;

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
                ))
            {
                return false;
            }
            break;
        case e_ee_gif_event_type::reglist:
            if ((offset & 7u) != 0u ||
                offset + 8u > bytes ||
                e.m_register > 0xfu ||
                !write_register(next, e.m_register, e.m_word0))
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
            break;
        }
        default:
            return false;
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
