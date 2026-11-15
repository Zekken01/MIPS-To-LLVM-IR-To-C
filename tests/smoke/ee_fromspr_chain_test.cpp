#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_base = ee::c_ee_dmac::k_from_spr_base;
constexpr uint32_t k_to_spr = ee::c_ee_dmac::k_to_spr_base;
constexpr uint32_t k_spr = ee::ee_memory::k_scratchpad_base;
constexpr uint32_t k_d_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_mode_chain = 1u << 2u;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;
constexpr uint32_t k_tie = 0x80u;
constexpr uint32_t k_ch8 = 1u << 8u;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);

int fail(const char* error)
{
    std::fprintf(stderr, "EE fromSPR DMA chain: %s\n", error);
    return 1;
}

bool same(const ee::s_ee_u128& a, const ee::s_ee_u128& b)
{
    return a.m_lo == b.m_lo && a.m_hi == b.m_hi;
}

ee::s_ee_u128 read_quad(ee::s_ee_runtime* rt, uint32_t address)
{
    ee::s_ee_u128 out{};
    ee::ee_mem_read128(rt, address, &out);
    return out;
}

void put_quad(ee::s_ee_runtime* rt, uint32_t address,
              const ee::s_ee_u128& quad)
{
    ee::ee_mem_write128(rt, address, &quad);
}

void put_tag(ee::s_ee_runtime* rt, uint32_t sadr,
             uint32_t madr, uint32_t qwc, uint32_t id, bool irq)
{
    const uint32_t lo = (qwc & 0xffffu) |
        ((id & 7u) << 28u) | (irq ? 0x80000000u : 0u);
    const uint64_t tag = (static_cast<uint64_t>(madr) << 32u) | lo;
    ee::ee_mem_write64(rt, k_spr + (sadr & 0x3ff0u), tag);
}

void program(ee::s_ee_runtime* rt, uint32_t base, uint32_t madr,
             uint32_t qwc, uint32_t sadr, uint32_t tadr)
{
    ee::ee_mem_write32(rt, base + 0x10u, madr);
    ee::ee_mem_write32(rt, base + 0x20u, qwc);
    ee::ee_mem_write32(rt, base + 0x30u, tadr);
    ee::ee_mem_write32(rt, base + 0x80u, sadr);
}
} // namespace

int main()
{
    ee::ee_memory memory(0x2000);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware{};
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);
    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    const ee::s_ee_u128 first{0x0123456789abcdefull, 0x1122334455667788ull};
    const ee::s_ee_u128 second{0x1020304050607080ull, 0x99aabbccddeeff00ull};
    const ee::s_ee_u128 sentinel{0xdeadbeefcafebabeull, 0x8899aabbccddeeffull};

    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;

    // Tags and payload span the end of the SPR. Each tag consumes a
    // 16-byte slot; the following QWC payload is copied to tag.MADR.
    // First tag ID=1 is nonterminating; second tag ID=7 ends the chain.
    put_tag(&runtime, 0x3fd0u, 0x400u, 1u, 1u, false);
    put_quad(&runtime, k_spr + 0x3fe0u, first);
    put_tag(&runtime, 0x3ff0u, 0x500u, 1u, 7u, false);
    put_quad(&runtime, k_spr, second);
    ee::ee_mem_write32(&runtime, k_d_stat, k_ch8 << 16u);
    program(&runtime, k_base, 0x100u, 0u, 0x3fd0u, 0x1230u);
    ee::ee_mem_write32(&runtime, k_base, k_mode_chain | k_str);

    if (!same(read_quad(&runtime, 0x400u), first) ||
        !same(read_quad(&runtime, 0x500u), second) ||
        ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x510u ||
        ee::ee_mem_read32(&runtime, k_base + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, k_base + 0x30u) != 0x1230u ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0x10u ||
        (ee::ee_mem_read32(&runtime, k_base) & 0xffff0000u) != 0x70000000u ||
        (ee::ee_mem_read32(&runtime, k_base) & k_str) != 0 ||
        (hardware.dmac().status() & k_ch8) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("tag decoding, two destinations, SPR wrap or completion IRQ");
    }

    ee::ee_mem_write32(&runtime, k_d_stat, k_ch8);
    if ((hardware.dmac().status() & k_ch8) != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0)
    {
        return fail("D_STAT completion acknowledgement");
    }

    // TIE + IRQ on a non-END tag terminates immediately after its data.
    // The next END tag is left untouched. Without TIE the same tag's IRQ
    // must NOT terminate the chain.
    hardware.reset();
    put_tag(&runtime, 0x100u, 0x700u, 1u, 1u, true);
    put_quad(&runtime, k_spr + 0x110u, first);
    put_tag(&runtime, 0x120u, 0x600u, 1u, 7u, false);
    put_quad(&runtime, k_spr + 0x130u, second);
    put_quad(&runtime, 0x600u, sentinel);
    program(&runtime, k_base, 0u, 0u, 0x100u, 0u);
    ee::ee_mem_write32(&runtime, k_base, k_mode_chain | k_tie | k_str);
    if (!same(read_quad(&runtime, 0x700u), first) ||
        !same(read_quad(&runtime, 0x600u), sentinel) ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0x120u ||
        ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x710u ||
        (ee::ee_mem_read32(&runtime, k_base) & 0xffff0000u) != 0x90000000u ||
        (hardware.dmac().status() & k_ch8) == 0u)
    {
        return fail("TIE + IRQ tag failed to stop before next tag");
    }

    hardware.reset();
    put_quad(&runtime, 0x600u, sentinel);
    program(&runtime, k_base, 0u, 0u, 0x100u, 0u);
    ee::ee_mem_write32(&runtime, k_base, k_mode_chain | k_str);
    if (!same(read_quad(&runtime, 0x600u), second) ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0x140u ||
        ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x610u ||
        (hardware.dmac().status() & k_ch8) == 0u)
    {
        return fail("IRQ tag incorrectly terminated with TIE disabled");
    }

    // A preloaded QWC segment can itself terminate based on CHCR tag ID.
    // No SPR tag should be read after copying the initial QWC segment.
    hardware.reset();
    put_quad(&runtime, k_spr + 0x3ff0u, first);
    program(&runtime, k_base, 0x800u, 1u, 0x3ff0u, 0x1000u);
    ee::ee_mem_write32(&runtime, k_base,
                       k_mode_chain | k_str | (7u << 28u));
    if (!same(read_quad(&runtime, 0x800u), first) ||
        ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x810u ||
        ee::ee_mem_read32(&runtime, k_base + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0u ||
        (ee::ee_mem_read32(&runtime, k_base) & k_str) != 0u ||
        (hardware.dmac().status() & k_ch8) == 0u)
    {
        return fail("preloaded QWC segment or CHCR END handling");
    }

    // A tag with QWC=0 must still advance past the tag and complete.
    hardware.reset();
    put_tag(&runtime, 0x200u, 0x810u, 0u, 7u, false);
    program(&runtime, k_base, 0u, 0u, 0x200u, 0u);
    ee::ee_mem_write32(&runtime, k_base, k_mode_chain | k_str);
    if (ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x810u ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0x210u ||
        (hardware.dmac().status() & k_ch8) == 0u)
    {
        return fail("zero-QWC END tag did not advance and complete");
    }

    // Preflight must reject an invalid second tag before the first valid
    // tag can write any RAM. A failed start retains STR without D_STAT.
    hardware.reset();
    put_tag(&runtime, 0x300u, 0x900u, 1u, 1u, false);
    put_quad(&runtime, k_spr + 0x310u, first);
    put_tag(&runtime, 0x320u, 0x1ff0u, 2u, 7u, false);
    put_quad(&runtime, k_spr + 0x330u, second);
    put_quad(&runtime, 0x900u, sentinel);
    put_quad(&runtime, 0x1ff0u, sentinel);
    program(&runtime, k_base, 0x1000u, 0u, 0x300u, 0x444u);
    ee::ee_mem_write32(&runtime, k_base, k_mode_chain | k_str);
    if (!same(read_quad(&runtime, 0x900u), sentinel) ||
        !same(read_quad(&runtime, 0x1ff0u), sentinel) ||
        ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x1000u ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0x300u ||
        ee::ee_mem_read32(&runtime, k_base + 0x30u) != 0x444u ||
        (ee::ee_mem_read32(&runtime, k_base) & k_str) == 0u ||
        (hardware.dmac().status() & k_ch8) != 0u)
    {
        return fail("failed chain wrote a preceding valid tag or completed");
    }

    // A preloaded invalid QWC segment also fails atomically.
    hardware.reset();
    put_quad(&runtime, 0x1ff0u, sentinel);
    program(&runtime, k_base, 0x1ff0u, 2u, 0x310u, 0u);
    ee::ee_mem_write32(&runtime, k_base,
                       k_mode_chain | k_str | (7u << 28u));
    if (!same(read_quad(&runtime, 0x1ff0u), sentinel) ||
        ee::ee_mem_read32(&runtime, k_base + 0x20u) != 2u ||
        ee::ee_mem_read32(&runtime, k_base + 0x10u) != 0x1ff0u ||
        ee::ee_mem_read32(&runtime, k_base + 0x80u) != 0x310u ||
        (ee::ee_mem_read32(&runtime, k_base) & k_str) == 0u ||
        (hardware.dmac().status() & k_ch8) != 0u)
    {
        return fail("invalid initial QWC partially copied or advanced");
    }

    // toSPR chain mode is a separate operation and remains unsupported.
    hardware.reset();
    program(&runtime, k_to_spr, 0x900u, 1u, 0x500u, 0u);
    ee::ee_mem_write32(&runtime, k_to_spr, k_mode_chain | k_str);
    if ((ee::ee_mem_read32(&runtime, k_to_spr) & k_str) == 0u ||
        (hardware.dmac().status() & (1u << 9u)) != 0u)
    {
        return fail("unsupported toSPR chain mode falsely completed");
    }

    // Empty scratchpad contains no END tags. The 4096-tag guard must
    // reject the chain without writing or raising an interrupt.
    {
        ee::ee_memory empty_memory(0x1000u);
        ee::c_ee_hw_bus empty_hardware{};
        empty_hardware.bind_memory(&empty_memory);
        ee::s_ee_runtime empty_runtime{};
        empty_runtime.m_user = &empty_memory;
        empty_runtime.m_mmio_user = &empty_hardware;
        empty_runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
        empty_runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;
        program(&empty_runtime, k_base, 0x80u, 0u, 0u, 0u);
        ee::ee_mem_write32(&empty_runtime, k_base, k_mode_chain | k_str);
        if ((ee::ee_mem_read32(&empty_runtime, k_base) & k_str) == 0u ||
            ee::ee_mem_read32(&empty_runtime, k_base + 0x80u) != 0u ||
            empty_hardware.dmac().status() != 0u)
        {
            return fail("unterminated circular chain fabricated completion");
        }
    }

    std::puts("EE fromSPR chain tags / wrap / preflight / TIE IRQ: PASS");
    return 0;
}
