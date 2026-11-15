#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_base = ee::c_ee_dmac::k_to_spr_base;
constexpr uint32_t k_spr = ee::ee_memory::k_scratchpad_base;
constexpr uint32_t k_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;
constexpr uint32_t k_chain = 1u << 2u;
constexpr uint32_t k_tie = 0x80u;
constexpr uint32_t k_ch9 = 1u << 9u;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);

int fail(const char* message)
{
    std::fprintf(stderr, "EE toSPR DMA chain: %s\n", message);
    return 1;
}

void put_quad(ee::s_ee_runtime* runtime, uint32_t addr, const ee::s_ee_u128& quad)
{
    ee::ee_mem_write128(runtime, addr, &quad);
}

ee::s_ee_u128 get_quad(ee::s_ee_runtime* runtime, uint32_t addr)
{
    ee::s_ee_u128 result{};
    ee::ee_mem_read128(runtime, addr, &result);
    return result;
}

bool eq(const ee::s_ee_u128& x, const ee::s_ee_u128& y)
{
    return x.m_lo == y.m_lo && x.m_hi == y.m_hi;
}

void tag(ee::s_ee_runtime* runtime, uint32_t where,
         uint32_t qwc, uint32_t id, uint32_t address, bool irq = false)
{
    const uint32_t low = (qwc & 0xffffu) |
        ((id & 7u) << 28u) | (irq ? 0x80000000u : 0u);
    ee::ee_mem_write64(runtime, where,
        (static_cast<uint64_t>(address) << 32u) | low);
}

void setup(ee::s_ee_runtime* runtime, uint32_t madr, uint32_t qwc,
           uint32_t tadr, uint32_t sadr)
{
    ee::ee_mem_write32(runtime, k_base + 0x10u, madr);
    ee::ee_mem_write32(runtime, k_base + 0x20u, qwc);
    ee::ee_mem_write32(runtime, k_base + 0x30u, tadr);
    ee::ee_mem_write32(runtime, k_base + 0x80u, sadr);
}

uint32_t read(ee::s_ee_runtime* runtime, uint32_t offset)
{
    return ee::ee_mem_read32(runtime, k_base + offset);
}

void start(ee::s_ee_runtime* runtime, uint32_t flags = 0u)
{
    ee::ee_mem_write32(runtime, k_base, k_chain | k_str | flags);
}
} // namespace

int main()
{
    ee::ee_memory ram(0x2000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware{};
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&ram);
    ee::s_ee_runtime runtime{};
    runtime.m_user = &ram;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    const ee::s_ee_u128 a{0x1020304050607080ull, 0x0102030405060708ull};
    const ee::s_ee_u128 b{0xaabbccddeeff0011ull, 0x8877665544332211ull};
    const ee::s_ee_u128 c{0x0123456789abcdefull, 0xfedcba9876543210ull};
    const ee::s_ee_u128 sentinel{0xdeadbeefcafebabeull, 0xd00dd00d12345678ull};

    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;

    // CNT copies inline and advances TADR to the next tag; END consumes
    // inline data but does not advance TADR (reference run_chain).
    tag(&runtime, 0x100u, 1u, 1u, 0u);
    put_quad(&runtime, 0x110u, a);
    tag(&runtime, 0x120u, 1u, 7u, 0u);
    put_quad(&runtime, 0x130u, b);
    ee::ee_mem_write32(&runtime, k_stat, k_ch9 << 16u);
    setup(&runtime, 0u, 0u, 0x100u, 0x3ff0u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0x3ff0u), a) ||
        !eq(get_quad(&runtime, k_spr), b) ||
        read(&runtime, 0x10u) != 0x140u ||
        read(&runtime, 0x20u) != 0u ||
        read(&runtime, 0x30u) != 0x120u ||
        read(&runtime, 0x80u) != 0x10u ||
        (read(&runtime, 0u) & k_str) != 0u ||
        (read(&runtime, 0u) & 0xffff0000u) != 0x70000000u ||
        (hardware.dmac().status() & k_ch9) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("CNT/END chain, TADR, SPR wrap or completion IP3");
    }
    ee::ee_mem_write32(&runtime, k_stat, k_ch9);
    if ((cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("D_STAT ack failed after chained transfer");
    }

    // NEXT: jump to a distant tag, not the inline successor.
    hardware.reset();
    tag(&runtime, 0x200u, 1u, 2u, 0x300u);
    put_quad(&runtime, 0x210u, a);
    tag(&runtime, 0x220u, 1u, 7u, 0u);
    put_quad(&runtime, 0x230u, sentinel);
    tag(&runtime, 0x300u, 1u, 7u, 0u);
    put_quad(&runtime, 0x310u, b);
    setup(&runtime, 0u, 0u, 0x200u, 0x400u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0x400u), a) ||
        !eq(get_quad(&runtime, k_spr + 0x410u), b) ||
        read(&runtime, 0x30u) != 0x300u ||
        read(&runtime, 0x10u) != 0x320u ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("NEXT branch did not use target tag");
    }

    // REF and REFS fetch external physical RAM while tag traversal
    // continues sequentially. REFE fetches external RAM and terminates.
    hardware.reset();
    put_quad(&runtime, 0x700u, a);
    put_quad(&runtime, 0x710u, b);
    put_quad(&runtime, 0x720u, c);
    tag(&runtime, 0x600u, 1u, 3u, 0x700u);
    tag(&runtime, 0x610u, 1u, 4u, 0x710u);
    tag(&runtime, 0x620u, 1u, 0u, 0x720u);
    setup(&runtime, 0u, 0u, 0x600u, 0x500u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0x500u), a) ||
        !eq(get_quad(&runtime, k_spr + 0x510u), b) ||
        !eq(get_quad(&runtime, k_spr + 0x520u), c) ||
        read(&runtime, 0x30u) != 0x630u ||
        read(&runtime, 0x10u) != 0x730u ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("REF/REFS/REFE external payload or termination");
    }

    // CALL pushes the address following its inline payload, RET pops
    // it, then the final END resumes after that CALL.
    hardware.reset();
    tag(&runtime, 0x800u, 1u, 5u, 0x900u);
    put_quad(&runtime, 0x810u, a);
    tag(&runtime, 0x820u, 1u, 7u, 0u);
    put_quad(&runtime, 0x830u, c);
    tag(&runtime, 0x900u, 1u, 6u, 0u);
    put_quad(&runtime, 0x910u, b);
    setup(&runtime, 0u, 0u, 0x800u, 0x600u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0x600u), a) ||
        !eq(get_quad(&runtime, k_spr + 0x610u), b) ||
        !eq(get_quad(&runtime, k_spr + 0x620u), c) ||
        read(&runtime, 0x30u) != 0x820u ||
        read(&runtime, 0x10u) != 0x840u ||
        read(&runtime, 0x40u) != 0x820u ||
        (read(&runtime, 0u) & 0x30u) != 0u ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("CALL/RET payload ordering or saved return address");
    }

    // Two nested CALLs exercise ASR[1] and correct return-stack order.
    // Zero-QWC tags still change control flow.
    hardware.reset();
    tag(&runtime, 0x1000u, 0u, 5u, 0x1100u);
    tag(&runtime, 0x1010u, 0u, 7u, 0u);
    tag(&runtime, 0x1100u, 0u, 5u, 0x1200u);
    tag(&runtime, 0x1110u, 0u, 6u, 0u);
    tag(&runtime, 0x1200u, 0u, 6u, 0u);
    setup(&runtime, 0u, 0u, 0x1000u, 0x700u);
    start(&runtime);
    if (read(&runtime, 0x30u) != 0x1010u ||
        read(&runtime, 0x40u) != 0x1010u ||
        read(&runtime, 0x50u) != 0x1110u ||
        (read(&runtime, 0u) & 0x30u) != 0u ||
        (read(&runtime, 0u) & k_str) != 0u ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("nested CALL/RET return stack");
    }

    // RET with ASP=0 terminates a chain, even without an END tag.
    hardware.reset();
    tag(&runtime, 0xa00u, 1u, 6u, 0u);
    put_quad(&runtime, 0xa10u, a);
    setup(&runtime, 0u, 0u, 0xa00u, 0x800u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0x800u), a) ||
        read(&runtime, 0x30u) != 0xa00u ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("RET with empty stack did not terminate");
    }

    // TIE+IRQ interrupts a non-END tag after copying its payload.
    hardware.reset();
    tag(&runtime, 0xb00u, 1u, 3u, 0x700u, true);
    tag(&runtime, 0xb10u, 1u, 7u, 0u);
    put_quad(&runtime, 0xb20u, sentinel);
    put_quad(&runtime, k_spr + 0x900u, sentinel);
    setup(&runtime, 0u, 0u, 0xb00u, 0x900u);
    start(&runtime, k_tie);
    if (!eq(get_quad(&runtime, k_spr + 0x900u), a) ||
        eq(get_quad(&runtime, k_spr + 0x910u), sentinel) ||
        read(&runtime, 0x30u) != 0xb10u ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("TIE IRQ tag failed to terminate");
    }

    // With TIE disabled, the same IRQ tag must not terminate.
    hardware.reset();
    setup(&runtime, 0u, 0u, 0xb00u, 0xa00u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0xa00u), a) ||
        !eq(get_quad(&runtime, k_spr + 0xa10u), sentinel) ||
        (hardware.dmac().status() & k_ch9) == 0u)
    {
        return fail("TIE-disabled IRQ tag incorrectly terminated");
    }

    // Late invalid tag: first valid CNT must not partially write SPR.
    // Preserve registers and STR on preflight failure.
    hardware.reset();
    tag(&runtime, 0xc00u, 1u, 1u, 0u);
    put_quad(&runtime, 0xc10u, a);
    tag(&runtime, 0xc20u, 2u, 0u, 0x1ff0u);
    put_quad(&runtime, k_spr + 0xb00u, sentinel);
    setup(&runtime, 0x444u, 0u, 0xc00u, 0xb00u);
    start(&runtime);
    if (!eq(get_quad(&runtime, k_spr + 0xb00u), sentinel) ||
        read(&runtime, 0x10u) != 0x444u ||
        read(&runtime, 0x30u) != 0xc00u ||
        read(&runtime, 0x80u) != 0xb00u ||
        (read(&runtime, 0u) & k_str) == 0u ||
        (hardware.dmac().status() & k_ch9) != 0u)
    {
        return fail("late invalid source caused partial toSPR chain copy");
    }

    // Malformed tag pointers and a self-referencing NEXT cannot succeed.
    hardware.reset();
    setup(&runtime, 0u, 0u, 0x1ff8u, 0xb00u);
    start(&runtime);
    if ((read(&runtime, 0u) & k_str) == 0u ||
        read(&runtime, 0x30u) != 0x1ff8u ||
        hardware.dmac().status() != 0u)
    {
        return fail("invalid TADR accepted");
    }
    hardware.reset();
    tag(&runtime, 0xd00u, 0u, 2u, 0xd00u);
    setup(&runtime, 0u, 0u, 0xd00u, 0xb00u);
    start(&runtime);
    if ((read(&runtime, 0u) & k_str) == 0u ||
        read(&runtime, 0x30u) != 0xd00u ||
        hardware.dmac().status() != 0u)
    {
        return fail("unterminated cyclic NEXT chain accepted");
    }

    // Unbound memory must never fabricate a completion interrupt.
    hardware.reset();
    hardware.bind_memory(nullptr);
    setup(&runtime, 0u, 0u, 0x100u, 0u);
    start(&runtime);
    if ((read(&runtime, 0u) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("unbound toSPR DMA falsely completed");
    }
    hardware.bind_memory(&ram);

    hardware.reset();
    if (read(&runtime, 0x40u) != 0u ||
        read(&runtime, 0x50u) != 0u ||
        read(&runtime, 0x20u) != 0u ||
        read(&runtime, 0x30u) != 0u ||
        hardware.dmac().status() != 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("channel 9 CALL/RET stack did not reset");
    }

    std::puts("EE toSPR chains: CNT/NEXT/REF/CALL/RET/END/IRQ/atomicity PASS");
    return 0;
}
