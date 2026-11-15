#include "ee_memory.h"
#include "ee_runtime.h"
#include <cstdlib>
#include <cstring>
#include <new>

namespace ee {

namespace
{
uint64_t load_le(const uint8_t* bytes, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
    {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    }
    return value;
}

void store_le(uint8_t* bytes, size_t count, uint64_t value)
{
    for (size_t i = 0; i < count; ++i)
    {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}
} // namespace

ee_memory::ee_memory(size_t size)
    : m_size(size)
{
    m_data = static_cast<uint8_t*>(std::malloc(size));
    if (m_data) {
        std::memset(m_data, 0, size);
    }
}

ee_memory::~ee_memory()
{
    std::free(m_data);
    m_data = nullptr;
}

void ee_memory::set_kseg_ram_aliases(bool enabled)
{
    m_kseg_ram_aliases = enabled;
}

uint32_t ee_memory::normalize_ram_address(uint32_t addr) const
{
    if (m_kseg_ram_aliases && (addr & 0xc0000000u) == 0x80000000u)
    {
        const uint32_t physical = addr & 0x1fffffffu;
        if (static_cast<size_t>(physical) < m_size)
        {
            return physical;
        }
    }
    return addr;
}


const uint8_t* ee_memory::map_address(uint32_t addr, size_t len) const
{
    // Scratchpad has its own storage. It is NOT part of RAM and is not
    // affected by the optional KSEG0/KSEG1 physical-RAM mirror mode.
    if (addr >= k_scratchpad_base &&
        static_cast<uint64_t>(addr - k_scratchpad_base) < k_scratchpad_size)
    {
        const size_t offset = static_cast<size_t>(addr - k_scratchpad_base);
        return len <= k_scratchpad_size - offset ? m_scratchpad + offset : nullptr;
    }

    const size_t physical = normalize_ram_address(addr);
    if (m_data == nullptr || physical > m_size || len > m_size - physical)
    {
        return nullptr;
    }
    return m_data + physical;
}

uint8_t* ee_memory::map_address(uint32_t addr, size_t len)
{
    return const_cast<uint8_t*>(
        static_cast<const ee_memory*>(this)->map_address(addr, len)
    );
}

bool ee_memory::contains(uint32_t addr, size_t len) const
{
    return map_address(addr, len) != nullptr;
}

uint8_t ee_memory::read8(uint32_t addr)
{
    const uint8_t* bytes = map_address(addr, 1);
    return bytes != nullptr ? bytes[0] : 0;
}

uint16_t ee_memory::read16(uint32_t addr)
{
    const uint8_t* bytes = map_address(addr, 2);
    return bytes != nullptr ? static_cast<uint16_t>(load_le(bytes, 2)) : 0;
}

void ee_memory::read128(uint32_t addr, s_ee_u128* out)
{
    if (out == nullptr)
    {
        return;
    }

    const uint8_t* bytes = map_address(addr, 16);
    if (bytes == nullptr)
    {
        *out = {};
        return;
    }

    out->m_lo = load_le(bytes, 8);
    out->m_hi = load_le(bytes + 8, 8);
}

void ee_memory::write8(uint32_t addr, uint8_t value)
{
    if (uint8_t* bytes = map_address(addr, 1))
    {
        bytes[0] = value;
    }
}

void ee_memory::write16(uint32_t addr, uint16_t value)
{
    if (uint8_t* bytes = map_address(addr, 2))
    {
        store_le(bytes, 2, value);
    }
}

void ee_memory::write128(uint32_t addr, const s_ee_u128* value)
{
    if (value == nullptr)
    {
        return;
    }

    // Reject a cross-boundary quadword in its entirety.
    if (uint8_t* bytes = map_address(addr, 16))
    {
        store_le(bytes, 8, value->m_lo);
        store_le(bytes + 8, 8, value->m_hi);
    }
}

void ee_memory::set_ram_range(uint32_t base, uint32_t size)
{
    m_ram_base = base;
    m_ram_size = size;
    m_ram_configured = true;
}


bool ee_memory::in_ram_range(uint32_t addr, size_t len) const
{
    if (!m_ram_configured || m_data == nullptr)
    {
        return false;
    }

    // Verify both the configured range and the actual host buffer before
    // allowing the unchecked direct_* path. Do not accept wraparound.
    const size_t start = static_cast<size_t>(addr);
    const size_t range_start = static_cast<size_t>(m_ram_base);
    const size_t range_end = range_start + static_cast<size_t>(m_ram_size);
    return start >= range_start && start <= range_end &&
        len <= range_end - start &&
        start <= m_size && len <= m_size - start;
}

uint32_t ee_memory::direct_read32(uint32_t addr)
{
    return (uint32_t)(m_data[addr] | (m_data[addr + 1] << 8) |
                      (m_data[addr + 2] << 16) | (m_data[addr + 3] << 24));
}

void ee_memory::direct_write32(uint32_t addr, uint32_t v)
{
    m_data[addr]   = (uint8_t)(v & 0xff);
    m_data[addr+1] = (uint8_t)((v >> 8) & 0xff);
    m_data[addr+2] = (uint8_t)((v >> 16) & 0xff);
    m_data[addr+3] = (uint8_t)((v >> 24) & 0xff);
}

uint64_t ee_memory::direct_read64(uint32_t addr)
{
    uint64_t lo = direct_read32(addr);
    uint64_t hi = direct_read32(addr + 4);
    return lo | (hi << 32);
}

void ee_memory::direct_write64(uint32_t addr, uint64_t v)
{
    direct_write32(addr,   (uint32_t)(v & 0xffffffffu));
    direct_write32(addr+4, (uint32_t)((v >> 32) & 0xffffffffu));
}


uint32_t ee_memory::read32(uint32_t addr)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 4))
    {
        return direct_read32(physical);
    }

    const uint8_t* bytes = map_address(addr, 4);
    return bytes != nullptr ? static_cast<uint32_t>(load_le(bytes, 4)) : 0;
}

void ee_memory::write32(uint32_t addr, uint32_t value)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 4))
    {
        direct_write32(physical, value);
        return;
    }

    if (uint8_t* bytes = map_address(addr, 4))
    {
        store_le(bytes, 4, value);
    }
}

uint64_t ee_memory::read64(uint32_t addr)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 8))
    {
        return direct_read64(physical);
    }

    const uint8_t* bytes = map_address(addr, 8);
    return bytes != nullptr ? load_le(bytes, 8) : 0;
}

void ee_memory::write64(uint32_t addr, uint64_t value)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 8))
    {
        direct_write64(physical, value);
        return;
    }

    if (uint8_t* bytes = map_address(addr, 8))
    {
        store_le(bytes, 8, value);
    }
}

namespace
{
ee_memory* runtime_memory(s_ee_runtime* rt)
{
    return static_cast<ee_memory*>(rt ? rt->m_user : nullptr);
}
} // namespace

uint8_t ee_mem_read8(s_ee_runtime* rt, uint32_t addr)
{
    ee_memory* mem = runtime_memory(rt);
    return mem ? mem->read8(addr) : 0;
}

uint16_t ee_mem_read16(s_ee_runtime* rt, uint32_t addr)
{
    ee_memory* mem = runtime_memory(rt);
    return mem ? mem->read16(addr) : 0;
}

uint32_t ee_mem_read32(s_ee_runtime* rt, uint32_t addr)
{
    ee_memory* mem = runtime_memory(rt);
    return mem ? mem->read32(addr) : 0;
}

uint64_t ee_mem_read64(s_ee_runtime* rt, uint32_t addr)
{
    ee_memory* mem = runtime_memory(rt);
    return mem ? mem->read64(addr) : 0;
}

void ee_mem_read128(s_ee_runtime* rt, uint32_t addr, s_ee_u128* out)
{
    if (!out) return;
    ee_memory* mem = runtime_memory(rt);
    if (mem) mem->read128(addr, out);
    else *out = {};
}

void ee_mem_write8(s_ee_runtime* rt, uint32_t addr, uint8_t value)
{
    if (ee_memory* mem = runtime_memory(rt)) mem->write8(addr, value);
}

void ee_mem_write16(s_ee_runtime* rt, uint32_t addr, uint16_t value)
{
    if (ee_memory* mem = runtime_memory(rt)) mem->write16(addr, value);
}

void ee_mem_write32(s_ee_runtime* rt, uint32_t addr, uint32_t value)
{
    if (ee_memory* mem = runtime_memory(rt)) mem->write32(addr, value);
}

void ee_mem_write64(s_ee_runtime* rt, uint32_t addr, uint64_t value)
{
    if (ee_memory* mem = runtime_memory(rt)) mem->write64(addr, value);
}

void ee_mem_write128(s_ee_runtime* rt, uint32_t addr, const s_ee_u128* value)
{
    if (ee_memory* mem = runtime_memory(rt)) mem->write128(addr, value);
}

} // namespace ee
