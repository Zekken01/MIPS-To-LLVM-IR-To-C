#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_spr = ee::ee_memory::k_scratchpad_base;
constexpr uint32_t k_from_spr = ee::c_ee_dmac::k_from_spr_base;
constexpr uint32_t k_to_spr = ee::c_ee_dmac::k_to_spr_base;
constexpr uint32_t k_sqwc = ee::c_ee_dmac::k_sqwc_address;
constexpr uint32_t k_d_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;
constexpr uint32_t k_interleave = 2u << 2u;
constexpr uint32_t k_ch8 = 1u << 8u;
constexpr uint32_t k_ch9 = 1u << 9u;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);

int fail(const char* message)
{
    std::fprintf(stderr, "EE SPR DMA interleave: %s\n", message);
    return 1;
}

ee::s_ee_u128 read_quad(ee::s_ee_runtime* runtime, uint32_t address)
{
    ee::s_ee_u128 value{};
    ee::ee_mem_read128(runtime, address, &value);
    return value;
}

void write_quad(ee::s_ee_runtime* runtime, uint32_t address,
                const ee::s_ee_u128& value)
{
    ee::ee_mem_write128(runtime, address, &value);
}

bool equal(const ee::s_ee_u128& a, const ee::s_ee_u128& b)
{
    return a.m_lo == b.m_lo && a.m_hi == b.m_hi;
}

void program(ee::s_ee_runtime* runtime, uint32_t base,
             uint32_t madr, uint32_t qwc, uint32_t sadr)
{
    ee::ee_mem_write32(runtime, base + 0x10u, madr);
    ee::ee_mem_write32(runtime, base + 0x20u, qwc);
    ee::ee_mem_write32(runtime, base + 0x80u, sadr);
}

uint32_t reg(ee::s_ee_runtime* runtime, uint32_t address)
{
    return ee::ee_mem_read32(runtime, address);
}
} // namespace

int main()
{
    ee::ee_memory memory(0x2000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware{};
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;

    const ee::s_ee_u128 a{0x0123456789abcdefull, 0x0011223344556677ull};
    const ee::s_ee_u128 b{0x1122334455667788ull, 0x8899aabbccddeeffull};
    const ee::s_ee_u128 c{0x76543210fedcba98ull, 0xa0a1a2a3a4a5a6a7ull};
    const ee::s_ee_u128 d{0x0102030405060708ull, 0x1112131415161718ull};
    const ee::s_ee_u128 e{0xaabbccddeeff0011ull, 0x2233445566778899ull};
    const ee::s_ee_u128 sentinel{0xdeadbeefcafebabeull, 0xc001d00df00dbad0ull};

    // fromSPR: TQWC=2, SQWC=1, QWC=5. RAM addresses touched:
    // 0x200, 0x210, 0x230, 0x240, 0x260. The gaps are not overwritten.
    // SADR starts near the end of SPR and wraps after three qwords.
    write_quad(&runtime, k_spr + 0x3fd0u, a);
    write_quad(&runtime, k_spr + 0x3fe0u, b);
    write_quad(&runtime, k_spr + 0x3ff0u, c);
    write_quad(&runtime, k_spr + 0x0000u, d);
    write_quad(&runtime, k_spr + 0x0010u, e);
    write_quad(&runtime, 0x220u, sentinel);
    write_quad(&runtime, 0x250u, sentinel);

    const uint32_t sqwc_full = 0xa5000000u | (2u << 16u) | 1u;
    ee::ee_mem_write32(&runtime, k_sqwc, sqwc_full);
    if (reg(&runtime, k_sqwc) != sqwc_full)
    {
        return fail("D_SQWC did not preserve all writable register bits");
    }
    program(&runtime, k_from_spr, 0x200u, 5u, 0x3fd0u);
    ee::ee_mem_write32(&runtime, k_from_spr, k_interleave | k_str);

    if (!equal(read_quad(&runtime, 0x200u), a) ||
        !equal(read_quad(&runtime, 0x210u), b) ||
        !equal(read_quad(&runtime, 0x230u), c) ||
        !equal(read_quad(&runtime, 0x240u), d) ||
        !equal(read_quad(&runtime, 0x260u), e) ||
        !equal(read_quad(&runtime, 0x220u), sentinel) ||
        !equal(read_quad(&runtime, 0x250u), sentinel) ||
        reg(&runtime, k_from_spr + 0x10u) != 0x270u ||
        reg(&runtime, k_from_spr + 0x20u) != 0u ||
        reg(&runtime, k_from_spr + 0x80u) != 0x20u ||
        (reg(&runtime, k_from_spr) & k_str) != 0u ||
        (hardware.dmac().status() & k_ch8) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("fromSPR interleave copy, RAM gaps, SPR wrap, or masked completion");
    }

    // Enabling a mask AFTER a completed transfer must assert IP3.
    ee::ee_mem_write32(&runtime, k_d_stat, k_ch8 << 16u);
    if ((cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u ||
        hardware.dmac().pending() != k_ch8)
    {
        return fail("D_STAT mask failed to assert pending fromSPR completion");
    }
    ee::ee_mem_write32(&runtime, k_d_stat, k_ch8);
    if ((cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("D_STAT acknowledgement did not clear IP3");
    }

    // toSPR: TQWC=1, SQWC=2. Consume RAM at 0x500, 0x530, 0x560
    // and deposit consecutive qwords at SPR 0x3ff0, 0x0000, 0x0010.
    hardware.reset();
    write_quad(&runtime, 0x500u, a);
    write_quad(&runtime, 0x530u, b);
    write_quad(&runtime, 0x560u, c);
    write_quad(&runtime, 0x510u, sentinel);
    ee::ee_mem_write32(&runtime, k_d_stat, k_ch9 << 16u);
    ee::ee_mem_write32(&runtime, k_sqwc, (1u << 16u) | 2u);
    program(&runtime, k_to_spr, 0x500u, 3u, 0x3ff0u);
    ee::ee_mem_write32(&runtime, k_to_spr, k_interleave | k_str);

    if (!equal(read_quad(&runtime, k_spr + 0x3ff0u), a) ||
        !equal(read_quad(&runtime, k_spr + 0x0000u), b) ||
        !equal(read_quad(&runtime, k_spr + 0x0010u), c) ||
        !equal(read_quad(&runtime, 0x510u), sentinel) ||
        reg(&runtime, k_to_spr + 0x10u) != 0x590u ||
        reg(&runtime, k_to_spr + 0x20u) != 0u ||
        reg(&runtime, k_to_spr + 0x80u) != 0x20u ||
        (reg(&runtime, k_to_spr) & k_str) != 0u ||
        (hardware.dmac().status() & k_ch9) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("toSPR interleave copy, MADR skip after final QWC, or IRQ");
    }

    // Reference behavior: when MOD=2 but D_SQWC.TQWC=0, use ordinary
    // contiguous normal-mode DMA (and never evaluate modulo zero).
    hardware.reset();
    ee::ee_mem_write32(&runtime, k_sqwc, 0x000000ffu);
    write_quad(&runtime, k_spr + 0x100u, d);
    write_quad(&runtime, k_spr + 0x110u, e);
    program(&runtime, k_from_spr, 0x700u, 2u, 0x100u);
    ee::ee_mem_write32(&runtime, k_from_spr, k_interleave | k_str);
    if (!equal(read_quad(&runtime, 0x700u), d) ||
        !equal(read_quad(&runtime, 0x710u), e) ||
        reg(&runtime, k_from_spr + 0x10u) != 0x720u ||
        reg(&runtime, k_from_spr + 0x80u) != 0x120u ||
        reg(&runtime, k_from_spr + 0x20u) != 0u ||
        (reg(&runtime, k_from_spr) & k_str) != 0u ||
        (hardware.dmac().status() & k_ch8) == 0u)
    {
        return fail("zero-transfer SQWC did not fall back to contiguous DMA");
    }

    // A partial last group advances only one QWC; a full last group
    // advances the final SQWC gap too (as in run_interleave).
    hardware.reset();
    ee::ee_mem_write32(&runtime, k_sqwc, (2u << 16u) | 3u);
    program(&runtime, k_from_spr, 0x800u, 2u, 0x100u);
    ee::ee_mem_write32(&runtime, k_from_spr, k_interleave | k_str);
    if (reg(&runtime, k_from_spr + 0x10u) != 0x850u ||
        reg(&runtime, k_from_spr + 0x80u) != 0x120u ||
        !equal(read_quad(&runtime, 0x800u), d) ||
        !equal(read_quad(&runtime, 0x810u), e))
    {
        return fail("final complete group failed to include its trailing skip");
    }

    // Invalid THIRD RAM destination. Validate the entire sparse pattern
    // before copying any of the first two valid qwords into SPR.
    hardware.reset();
    ee::ee_mem_write32(&runtime, k_sqwc, (1u << 16u) | 1u);
    write_quad(&runtime, k_spr + 0x300u, sentinel);
    write_quad(&runtime, k_spr + 0x310u, sentinel);
    write_quad(&runtime, k_spr + 0x320u, sentinel);
    program(&runtime, k_to_spr, 0x1fc0u, 3u, 0x300u);
    ee::ee_mem_write32(&runtime, k_to_spr, k_interleave | k_str);
    if (!equal(read_quad(&runtime, k_spr + 0x300u), sentinel) ||
        !equal(read_quad(&runtime, k_spr + 0x310u), sentinel) ||
        !equal(read_quad(&runtime, k_spr + 0x320u), sentinel) ||
        reg(&runtime, k_to_spr + 0x10u) != 0x1fc0u ||
        reg(&runtime, k_to_spr + 0x20u) != 3u ||
        reg(&runtime, k_to_spr + 0x80u) != 0x300u ||
        (reg(&runtime, k_to_spr) & k_str) == 0u ||
        (hardware.dmac().status() & k_ch9) != 0u)
    {
        return fail("late invalid toSPR RAM read partially modified scratchpad");
    }

    // The same guard is mandatory for fromSPR sparse RAM destinations.
    // Hardware reset clears D_SQWC, so restore TQWC=1/SQWC=1 before
    // starting this deliberately invalid interleave transfer.
    hardware.reset();
    ee::ee_mem_write32(&runtime, k_sqwc, (1u << 16u) | 1u);
    write_quad(&runtime, 0x1fc0u, sentinel);
    write_quad(&runtime, 0x1fe0u, sentinel);
    program(&runtime, k_from_spr, 0x1fc0u, 3u, 0x100u);
    ee::ee_mem_write32(&runtime, k_from_spr, k_interleave | k_str);
    if (!equal(read_quad(&runtime, 0x1fc0u), sentinel) ||
        !equal(read_quad(&runtime, 0x1fe0u), sentinel) ||
        reg(&runtime, k_from_spr + 0x10u) != 0x1fc0u ||
        reg(&runtime, k_from_spr + 0x20u) != 3u ||
        (reg(&runtime, k_from_spr) & k_str) == 0u ||
        (hardware.dmac().status() & k_ch8) != 0u)
    {
        return fail("late invalid fromSPR RAM write partially modified RAM");
    }

    // Unbound memory, width restrictions, physical alias and reset.
    hardware.reset();
    hardware.bind_memory(nullptr);
    ee::ee_mem_write32(&runtime, k_sqwc, (1u << 16u) | 1u);
    program(&runtime, k_from_spr, 0x100u, 1u, 0x100u);
    ee::ee_mem_write32(&runtime, k_from_spr, k_interleave | k_str);
    if (reg(&runtime, k_from_spr + 0x10u) != 0x100u ||
        reg(&runtime, k_from_spr + 0x20u) != 1u ||
        (reg(&runtime, k_from_spr) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("unbound interleave memory fabricated a completion");
    }

    hardware.bind_memory(&memory);
    memory.set_kseg_ram_aliases(true);
    ee::ee_mem_write32(&runtime, 0xb000e030u, 0x00010002u);
    if (reg(&runtime, k_sqwc) != 0x00010002u ||
        ee::ee_mem_read32(&runtime, 0xb000e030u) != 0x00010002u)
    {
        return fail("KSEG1 D_SQWC alias");
    }
    const uint32_t before_sqwc = reg(&runtime, k_sqwc);
    ee::ee_mem_write8(&runtime, k_sqwc, 0xffu);
    ee::ee_mem_write16(&runtime, k_sqwc, 0xffffu);
    ee::ee_mem_write64(&runtime, k_sqwc, ~uint64_t{0});
    ee::s_ee_u128 quad{~uint64_t{0}, ~uint64_t{0}};
    ee::ee_mem_write128(&runtime, k_sqwc, &quad);
    ee::ee_mem_read128(&runtime, k_sqwc, &quad);
    if (reg(&runtime, k_sqwc) != before_sqwc ||
        ee::ee_mem_read8(&runtime, k_sqwc) != 0u ||
        ee::ee_mem_read16(&runtime, k_sqwc) != 0u ||
        ee::ee_mem_read64(&runtime, k_sqwc) != 0u ||
        quad.m_lo != 0u || quad.m_hi != 0u)
    {
        return fail("unsupported D_SQWC widths altered register state");
    }

    hardware.reset();
    if (reg(&runtime, k_sqwc) != 0u ||
        reg(&runtime, k_from_spr + 0x10u) != 0u ||
        reg(&runtime, k_from_spr + 0x20u) != 0u ||
        reg(&runtime, k_to_spr + 0x10u) != 0u ||
        hardware.dmac().status() != 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("D_SQWC and DMA channels did not reset");
    }

    std::puts("EE D_SQWC interleave SPR DMA channels 8/9: PASS");
    return 0;
}
