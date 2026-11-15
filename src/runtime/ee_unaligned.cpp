#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
namespace
{
uint64_t mask_below(uint32_t bits)
{
    return bits >= 64u ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1u);
}

uint32_t aligned_word(uint32_t address)
{
    return address & ~uint32_t{3};
}

uint32_t aligned_doubleword(uint32_t address)
{
    return address & ~uint32_t{7};
}
} // namespace

// Byte-lane merge rules cross-checked with ps2-web-recomp's
// runtime/include/ps2/memory.hpp (lwl/lwr/ldl/ldr/swl/swr/sdl/sdr).
// The helpers intentionally read the aligned containing word/doubleword,
// not an unaligned multi-byte span that could cross the mapped region.

extern "C" uint64_t ee_mem_lwl(s_ee_runtime* runtime, uint32_t address, uint64_t old_value)
{
    const uint32_t shift = (3u - (address & 3u)) * 8u;
    const uint32_t memory = ee_mem_read32(runtime, aligned_word(address));
    const uint32_t merged = (memory << shift) |
        (static_cast<uint32_t>(old_value) & static_cast<uint32_t>(mask_below(shift)));
    // LWL always sign-extends the new 32-bit result into the low 64.
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(merged)));
}

extern "C" uint64_t ee_mem_lwr(s_ee_runtime* runtime, uint32_t address, uint64_t old_value)
{
    const uint32_t shift = (address & 3u) * 8u;
    const uint32_t memory = ee_mem_read32(runtime, aligned_word(address));
    const uint32_t merged = (memory >> shift) |
        (static_cast<uint32_t>(old_value) & ~static_cast<uint32_t>(mask_below(32u - shift)));

    // The R5900's LWR preserves bits 32..63 for a partial merge, but
    // sign-extends a fully replaced word at aligned offset zero.
    if (shift == 0u)
    {
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(merged)));
    }

    return (old_value & 0xffffffff00000000ull) | merged;
}

extern "C" uint64_t ee_mem_ldl(s_ee_runtime* runtime, uint32_t address, uint64_t old_value)
{
    const uint32_t shift = (7u - (address & 7u)) * 8u;
    const uint64_t memory = ee_mem_read64(runtime, aligned_doubleword(address));
    return (memory << shift) | (old_value & mask_below(shift));
}

extern "C" uint64_t ee_mem_ldr(s_ee_runtime* runtime, uint32_t address, uint64_t old_value)
{
    const uint32_t shift = (address & 7u) * 8u;
    const uint64_t memory = ee_mem_read64(runtime, aligned_doubleword(address));
    return (memory >> shift) | (old_value & ~mask_below(64u - shift));
}

extern "C" void ee_mem_swl(s_ee_runtime* runtime, uint32_t address, uint64_t source)
{
    const uint32_t shift = (3u - (address & 3u)) * 8u;
    const uint32_t at = aligned_word(address);
    const uint32_t previous = ee_mem_read32(runtime, at);
    const uint32_t merged = (static_cast<uint32_t>(source) >> shift) |
        (previous & ~static_cast<uint32_t>(mask_below(32u - shift)));
    ee_mem_write32(runtime, at, merged);
}

extern "C" void ee_mem_swr(s_ee_runtime* runtime, uint32_t address, uint64_t source)
{
    const uint32_t shift = (address & 3u) * 8u;
    const uint32_t at = aligned_word(address);
    const uint32_t previous = ee_mem_read32(runtime, at);
    const uint32_t merged = (static_cast<uint32_t>(source) << shift) |
        (previous & static_cast<uint32_t>(mask_below(shift)));
    ee_mem_write32(runtime, at, merged);
}

extern "C" void ee_mem_sdl(s_ee_runtime* runtime, uint32_t address, uint64_t source)
{
    const uint32_t shift = (7u - (address & 7u)) * 8u;
    const uint32_t at = aligned_doubleword(address);
    const uint64_t previous = ee_mem_read64(runtime, at);
    const uint64_t merged = (source >> shift) | (previous & ~mask_below(64u - shift));
    ee_mem_write64(runtime, at, merged);
}

extern "C" void ee_mem_sdr(s_ee_runtime* runtime, uint32_t address, uint64_t source)
{
    const uint32_t shift = (address & 7u) * 8u;
    const uint32_t at = aligned_doubleword(address);
    const uint64_t previous = ee_mem_read64(runtime, at);
    const uint64_t merged = (source << shift) | (previous & mask_below(shift));
    ee_mem_write64(runtime, at, merged);
}
} // namespace ee
