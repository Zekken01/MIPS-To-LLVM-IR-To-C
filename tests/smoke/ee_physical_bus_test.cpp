#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_intc_mask = 0x1000f010u;
constexpr uint32_t k_gs_csr = 0x12001000u;
constexpr uint32_t k_vif_fifo = 0x10004000u;

struct s_mock_io
{
    uint32_t m_reads = 0;
    uint32_t m_writes = 0;
    uint32_t m_last_address = 0;
    uint32_t m_last_width = 0;
    ee::s_ee_u128 m_last_value{};
};

bool is_known_register(uint32_t address)
{
    return address == k_intc_mask || address == k_gs_csr || address == k_vif_fifo;
}

bool read_mmio(void* user, uint32_t address, uint32_t width, ee::s_ee_u128* value)
{
    s_mock_io* io = static_cast<s_mock_io*>(user);
    ++io->m_reads;
    io->m_last_address = address;
    io->m_last_width = width;
    if (!is_known_register(address))
    {
        return false;
    }

    value->m_lo = 0x8877665544332211ull;
    value->m_hi = 0xfedcba9876543210ull;
    return true;
}

bool write_mmio(void* user, uint32_t address, uint32_t width,
                const ee::s_ee_u128* value)
{
    s_mock_io* io = static_cast<s_mock_io*>(user);
    ++io->m_writes;
    io->m_last_address = address;
    io->m_last_width = width;
    if (!is_known_register(address))
    {
        return false;
    }

    io->m_last_value = *value;
    return true;
}

int fail(const char* text)
{
    std::fprintf(stderr, "EE direct physical/MMIO bus: %s\n", text);
    return 1;
}
} // namespace

int main()
{
    ee::ee_memory memory(4096);
    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    ee::s_ee_vu0_state vu0{};
    runtime.m_vu0_state = &vu0;

    s_mock_io io{};
    runtime.m_mmio_user = &io;
    runtime.m_mmio_read = read_mmio;
    runtime.m_mmio_write = write_mmio;

    memory.write32(0x20u, 0x12345678u);
    if (memory.read32(0x20000020u) != 0 ||
        memory.read32(0x30000020u) != 0 ||
        ee::ee_mem_read32(&runtime, 0x9000f010u) != 0 ||
        io.m_reads != 0)
    {
        return fail("direct aliases must remain opt-in");
    }

    memory.set_kseg_ram_aliases(true);
    if (memory.read32(0x80000020u) != 0x12345678u ||
        memory.read32(0xa0000020u) != 0x12345678u ||
        memory.read32(0x20000020u) != 0x12345678u ||
        memory.read32(0x30000020u) != 0x12345678u)
    {
        return fail("KSEG and 0x2/0x3 physical RAM aliases");
    }

    ee::ee_mem_write32(&runtime, 0x30000024u, 0xdeadbeefu);
    if (memory.read32(0x24u) != 0xdeadbeefu ||
        memory.contains(0x20001000u, 4) ||
        memory.contains(0x32000000u, 4))
    {
        return fail("direct alias write or out-of-range RAM validation");
    }

    // Direct-mapped KSEG device address translates to the same physical
    // register as a normal bus access. Width/physical address are preserved.
    if (ee::ee_mem_read8(&runtime, 0x9000f010u) != 0x11u ||
        io.m_last_address != k_intc_mask || io.m_last_width != 1 ||
        ee::ee_mem_read16(&runtime, 0xb000f010u) != 0x2211u ||
        io.m_last_address != k_intc_mask || io.m_last_width != 2 ||
        ee::ee_mem_read32(&runtime, k_intc_mask) != 0x44332211u ||
        io.m_last_width != 4 ||
        ee::ee_mem_read64(&runtime, 0xb000f010u) != 0x8877665544332211ull ||
        io.m_last_width != 8)
    {
        return fail("MMIO scalar width/alias/physical-address dispatch");
    }

    const uint32_t reads_before_128 = io.m_reads;
    ee::s_ee_u128 result{};
    ee::ee_mem_read128(&runtime, k_vif_fifo, &result);
    if (io.m_reads != reads_before_128 + 1 ||
        io.m_last_address != k_vif_fifo || io.m_last_width != 16 ||
        result.m_lo != 0x8877665544332211ull ||
        result.m_hi != 0xfedcba9876543210ull)
    {
        return fail("128-bit FIFO read split or value corrupted");
    }

    ee::ee_mem_write8(&runtime, k_intc_mask, 0xabu);
    if (io.m_last_address != k_intc_mask ||
        io.m_last_width != 1 || io.m_last_value.m_lo != 0xabu)
    {
        return fail("8-bit register write");
    }
    ee::ee_mem_write16(&runtime, k_intc_mask, 0x1234u);
    if (io.m_last_width != 2 || io.m_last_value.m_lo != 0x1234u)
    {
        return fail("16-bit register write");
    }
    ee::ee_mem_write32(&runtime, 0xb000f010u, 0xdeadbeefu);
    if (io.m_last_width != 4 || io.m_last_address != k_intc_mask ||
        io.m_last_value.m_lo != 0xdeadbeefu)
    {
        return fail("32-bit KSEG MMIO write");
    }
    ee::ee_mem_write64(&runtime, k_gs_csr, 0x0123456789abcdefull);
    if (io.m_last_width != 8 || io.m_last_address != k_gs_csr ||
        io.m_last_value.m_lo != 0x0123456789abcdefull)
    {
        return fail("64-bit GS register write");
    }

    const ee::s_ee_u128 quad{0x0123456789abcdefull, 0xfedcba9876543210ull};
    const uint32_t writes_before_128 = io.m_writes;
    ee::ee_mem_write128(&runtime, k_vif_fifo, &quad);
    if (io.m_writes != writes_before_128 + 1 ||
        io.m_last_address != k_vif_fifo || io.m_last_width != 16 ||
        io.m_last_value.m_lo != quad.m_lo ||
        io.m_last_value.m_hi != quad.m_hi)
    {
        return fail("128-bit FIFO write split or value corrupted");
    }

    // No MMIO callbacks must be generated for the existing VU0 apertures.
    const uint32_t writes_before_vu = io.m_writes;
    ee::ee_mem_write32(&runtime, 0xb1000000u, 0xaabbccddu);
    ee::ee_mem_write32(&runtime, 0x11004020u, 0x11223344u);
    if (io.m_writes != writes_before_vu ||
        ee::ee_mem_read32(&runtime, 0x11000000u) != 0xaabbccddu ||
        ee::ee_mem_read32(&runtime, 0xb1004020u) != 0x11223344u)
    {
        return fail("VU0 data/micro physical aliases or MMIO priority");
    }

    // A transfer starting in VU0 cannot spill into an MMIO callback.
    const uint32_t reads_before_cross = io.m_reads;
    const uint32_t writes_before_cross = io.m_writes;
    ee::ee_mem_write32(&runtime, 0x11000ffeu, 0xffffffffu);
    ee::ee_mem_read64(&runtime, 0x11004ffcu);
    ee::ee_mem_read128(&runtime, 0x12fffffcu, &result);
    if (io.m_reads != reads_before_cross ||
        io.m_writes != writes_before_cross ||
        ee::ee_mem_read32(&runtime, 0x11000000u) != 0xaabbccddu)
    {
        return fail("cross-aperture read/write dispatched or mutated VU0");
    }

    // A named but unimplemented hardware register must not behave like RAM.
    result.m_lo = ~uint64_t{0};
    result.m_hi = ~uint64_t{0};
    ee::ee_mem_read128(&runtime, 0x1000f100u, &result);
    if (result.m_lo != 0 || result.m_hi != 0)
    {
        return fail("unimplemented 128-bit MMIO read did not fail closed");
    }
    const uint32_t writes_before_unknown = io.m_writes;
    ee::ee_mem_write32(&runtime, 0x1000f100u, 0xffffffffu);
    if (io.m_writes != writes_before_unknown + 1 ||
        memory.read32(0x20u) != 0x12345678u)
    {
        return fail("unimplemented device wrote main memory");
    }

    // Physical aliases can be disabled again; physical registers still work.
    memory.set_kseg_ram_aliases(false);
    const uint32_t reads_before_disabled = io.m_reads;
    if (ee::ee_mem_read32(&runtime, 0x9000f010u) != 0 ||
        ee::ee_mem_read32(&runtime, 0xb000f010u) != 0 ||
        io.m_reads != reads_before_disabled ||
        memory.read32(0x20000020u) != 0 ||
        ee::ee_mem_read32(&runtime, k_intc_mask) != 0x44332211u)
    {
        return fail("disabled aliases still resolve or physical MMIO stopped");
    }

    runtime.m_mmio_read = nullptr;
    runtime.m_mmio_write = nullptr;
    ee::ee_mem_write32(&runtime, k_intc_mask, 0xaaaaaaaau);
    if (ee::ee_mem_read32(&runtime, k_intc_mask) != 0 ||
        ee::ee_mem_read64(&runtime, 0x1fc00000u) != 0)
    {
        return fail("absent MMIO callbacks returned invented hardware state");
    }

    std::puts("EE direct aliases / side-effectful MMIO callback widths: PASS");
    return 0;
}
