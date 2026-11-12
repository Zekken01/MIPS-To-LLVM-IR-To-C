#pragma once
#include "ee_state.h"
#include <cstdint>
#include <cstddef>

namespace ee {

// Minimal in-memory guest RAM backing the memory helper ABI for the smoke
// test. The real runtime implements translation, MMIO, address errors, etc.
class ee_memory
{
public:
    explicit ee_memory(size_t size);
    ~ee_memory();

    uint8_t  read8 (uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    uint64_t read64(uint32_t addr);
    void     read128(uint32_t addr, s_ee_u128* out);

    void     write8 (uint32_t addr, uint8_t  v);
    void     write16(uint32_t addr, uint16_t v);
    void     write32(uint32_t addr, uint32_t v);
    void     write64(uint32_t addr, uint64_t v);
    void     write128(uint32_t addr, const s_ee_u128* v);

    bool contains(uint32_t addr, size_t len) const;

    // Opt-in EE KSEG0/KSEG1 direct-mapped RAM aliases. This does not
    // implement TLB translation or expose physical MMIO devices.
    void set_kseg_ram_aliases(bool enabled);

    // Direct-RAM fast path (Phase 7 optimisation). Marks a guest address
    // range as safe for direct host pointer access, bypassing bounds checks.
    // Accesses outside this range still go through the checked helpers.
    void set_ram_range(uint32_t base, uint32_t size);

    // Unchecked direct access; only valid for addresses within the RAM range
    // set via set_ram_range. Caller must ensure the address is in range.
    uint32_t direct_read32(uint32_t addr);
    void     direct_write32(uint32_t addr, uint32_t v);
    uint64_t direct_read64(uint32_t addr);
    void     direct_write64(uint32_t addr, uint64_t v);

private:
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
    uint32_t m_ram_base = 0;
    uint32_t m_ram_size = 0;
    bool m_ram_configured = false;
    bool m_kseg_ram_aliases = false;

    bool in_ram_range(uint32_t addr, size_t len) const;
    uint32_t normalize_ram_address(uint32_t addr) const;
};

} // namespace ee
