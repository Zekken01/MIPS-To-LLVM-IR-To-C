#include "ee_memory.h"
#include "ee_runtime.h"
#include <cstdlib>
#include <cstring>
#include <new>

namespace ee {

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

bool ee_memory::contains(uint32_t addr, size_t len) const
{
    return m_data && addr + len <= m_size;
}

uint8_t ee_memory::read8(uint32_t addr)
{
    return contains(addr, 1) ? m_data[addr] : 0;
}

uint16_t ee_memory::read16(uint32_t addr)
{
    if (!contains(addr, 2)) return 0;
    return (uint16_t)(m_data[addr] | (m_data[addr + 1] << 8));
}

void ee_memory::read128(uint32_t addr, s_ee_u128* out)
{
    if (out) {
        out->m_lo = read64(addr);
        out->m_hi = read64(addr + 8);
    }
}

void ee_memory::write8(uint32_t addr, uint8_t v)
{
    if (contains(addr, 1)) m_data[addr] = v;
}

void ee_memory::write16(uint32_t addr, uint16_t v)
{
    if (!contains(addr, 2)) return;
    m_data[addr]   = (uint8_t)(v & 0xff);
    m_data[addr+1] = (uint8_t)((v >> 8) & 0xff);
}

void ee_memory::write128(uint32_t addr, const s_ee_u128* v)
{
    if (!v) return;
    write64(addr,   v->m_lo);
    write64(addr+8, v->m_hi);
}

void ee_memory::set_ram_range(uint32_t base, uint32_t size)
{
    m_ram_base = base;
    m_ram_size = size;
    m_ram_configured = true;
}

bool ee_memory::in_ram_range(uint32_t addr, size_t len) const
{
    if (!m_ram_configured) return false;
    return addr >= m_ram_base && addr + len <= m_ram_base + m_ram_size;
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
    if (in_ram_range(addr, 4)) return direct_read32(addr);
    if (!contains(addr, 4)) return 0;
    return (uint32_t)(m_data[addr] | (m_data[addr + 1] << 8) |
                      (m_data[addr + 2] << 16) | (m_data[addr + 3] << 24));
}

void ee_memory::write32(uint32_t addr, uint32_t v)
{
    if (in_ram_range(addr, 4)) { direct_write32(addr, v); return; }
    if (!contains(addr, 4)) return;
    m_data[addr]   = (uint8_t)(v & 0xff);
    m_data[addr+1] = (uint8_t)((v >> 8) & 0xff);
    m_data[addr+2] = (uint8_t)((v >> 16) & 0xff);
    m_data[addr+3] = (uint8_t)((v >> 24) & 0xff);
}

uint64_t ee_memory::read64(uint32_t addr)
{
    if (in_ram_range(addr, 8)) return direct_read64(addr);
    if (!contains(addr, 8)) return 0;
    uint64_t lo = read32(addr);
    uint64_t hi = read32(addr + 4);
    return lo | (hi << 32);
}

void ee_memory::write64(uint32_t addr, uint64_t v)
{
    if (in_ram_range(addr, 8)) { direct_write64(addr, v); return; }
    write32(addr,   (uint32_t)(v & 0xffffffffu));
    write32(addr+4, (uint32_t)((v >> 32) & 0xffffffffu));
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
