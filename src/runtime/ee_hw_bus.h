#pragma once

#include "ee_dmac.h"
#include "ee_intc.h"

namespace ee
{
// Minimal host MMIO dispatcher. It only routes registers implemented by
// these device models; unsupported addresses do not acquire fake state.
class c_ee_hw_bus
{
public:
    void bind_cpu(s_ee_cpu_state* cpu)
    {
        m_intc.bind_cpu(cpu);
        m_dmac.bind_cpu(cpu);
    }

    void reset()
    {
        m_intc.reset();
        m_dmac.reset();
    }

    c_ee_intc& intc()
    {
        return m_intc;
    }

    c_ee_dmac& dmac()
    {
        return m_dmac;
    }

    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value)
    {
        if (user == nullptr)
        {
            return false;
        }

        c_ee_hw_bus* bus = static_cast<c_ee_hw_bus*>(user);
        return c_ee_intc::mmio_read(&bus->m_intc, address, width, value) ||
               c_ee_dmac::mmio_read(&bus->m_dmac, address, width, value);
    }

    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value)
    {
        if (user == nullptr)
        {
            return false;
        }

        c_ee_hw_bus* bus = static_cast<c_ee_hw_bus*>(user);
        return c_ee_intc::mmio_write(&bus->m_intc, address, width, value) ||
               c_ee_dmac::mmio_write(&bus->m_dmac, address, width, value);
    }

private:
    c_ee_intc m_intc;
    c_ee_dmac m_dmac;
};
} // namespace ee
