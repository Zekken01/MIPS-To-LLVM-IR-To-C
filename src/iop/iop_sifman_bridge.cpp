#include "iop_sifman_bridge.h"

#include <utility>
#include <vector>

namespace iop
{
namespace
{
bool create_stub(
    c_cpu& cpu, uint32_t pc, uint32_t service
)
{
    // MIPS I: lui v0,upper; ori v0,v0,lower; syscall; jr ra; nop
    // The HLE operation is only performed if an IOP thread actually
    // reaches this syscall after calling its resolved IRX import.
    const uint32_t code[] =
    {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0x00000000u
    };
    bool valid = true;
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        valid = valid && cpu.write32(pc + i * 4u, code[i]);
    }
    return valid;
}
} // namespace

c_sifman_bridge::c_sifman_bridge(
    c_kernel& kernel, ee::c_ee_sif& sif,
    ee::c_ee_dmac* dmac, ee::ee_memory* ee_ram
)
    : m_kernel(kernel), m_sif(sif),
      m_dmac(dmac), m_ee_ram(ee_ram)
{
}

bool c_sifman_bridge::install()
{
    // Validate before writing stubs. Duplicate library registrations
    // are rejected rather than silently overwriting an IOP library.
    if (!m_kernel.register_library("sifman", 23u, k_get_smflag_stub))
    {
        return false;
    }
    if (!m_kernel.register_library("sifman", 24u, k_set_smflag_stub))
    {
        return false;
    }
    if (!m_kernel.register_library("sifman", 7u, k_set_dma_stub))
    {
        return false;
    }
    return create_stub(
        m_kernel.cpu(), k_get_smflag_stub, k_get_smflag_syscall
    ) && create_stub(
        m_kernel.cpu(), k_set_smflag_stub, k_set_smflag_syscall
    ) && create_stub(
        m_kernel.cpu(), k_set_dma_stub, k_set_dma_syscall
    );
}

bool c_sifman_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    switch (service)
    {
    case k_set_dma_syscall:
    {
        // Guest sceSifSetDma receives a pointer to 16-byte
        // descriptors (IOP src, EE dst, byte count, attributes).
        // Unlike the reference's eager memcpy, validate and stage
        // every transfer before writing a single guest EE byte.
        if (m_dmac == nullptr || m_ee_ram == nullptr)
        {
            return false;
        }
        const uint32_t list = kernel.cpu().state().m_gpr[4];
        const uint32_t count = kernel.cpu().state().m_gpr[5];
        const uint32_t list_physical = list & 0x1fffffffu;
        const uint32_t list_region = list & 0xe0000000u;
        if (count == 0u || count > 64u ||
            (list & 3u) != 0u ||
            (list_region != 0u &&
             list_region != 0x80000000u &&
             list_region != 0xa0000000u) ||
            list_physical > c_cpu::k_ram_size - count * 16u)
        {
            return false;
        }

        ee::s_ee_u128 chcr{};
        if (!ee::c_ee_dmac::mmio_read(
                m_dmac, ee::c_ee_dmac::k_sif0_base,
                4u, &chcr) ||
            (chcr.m_lo & ee::c_ee_dmac::k_chcr_str) == 0u)
        {
            return false;
        }

        struct s_transfer
        {
            uint32_t m_ee_destination = 0u;
            std::vector<uint8_t> m_bytes;
        };
        std::vector<s_transfer> transfers;
        for (uint32_t index = 0u; index < count; ++index)
        {
            const uint32_t descriptor = list_physical + index * 16u;
            uint32_t source = 0u;
            uint32_t destination = 0u;
            uint32_t size = 0u;
            uint32_t attributes = 0u;
            if (!kernel.cpu().read32(descriptor, &source) ||
                !kernel.cpu().read32(descriptor + 4u, &destination) ||
                !kernel.cpu().read32(descriptor + 8u, &size) ||
                !kernel.cpu().read32(descriptor + 12u, &attributes))
            {
                return false;
            }

            const uint32_t source_physical = source & 0x1fffffffu;
            const uint32_t source_region = source & 0xe0000000u;
            const uint32_t destination_physical =
                destination & 0x1fffffffu;
            const uint32_t destination_region =
                destination & 0xe0000000u;
            const uint64_t rounded =
                (static_cast<uint64_t>(size) + 3u) & ~uint64_t(3u);
            if (size == 0u || size > 65536u ||
                attributes != 0u ||
                (source_region != 0u &&
                 source_region != 0x80000000u &&
                 source_region != 0xa0000000u) ||
                (destination_region != 0u &&
                 destination_region != 0x80000000u &&
                 destination_region != 0xa0000000u) ||
                (source_physical & 3u) != 0u ||
                (destination_physical & 3u) != 0u ||
                static_cast<uint64_t>(source_physical) + rounded >
                    c_cpu::k_ram_size ||
                !m_ee_ram->contains(destination_physical, size))
            {
                return false;
            }

            s_transfer transfer{};
            transfer.m_ee_destination = destination_physical;
            transfer.m_bytes.reserve(size);
            for (uint32_t offset = 0u; offset < size; offset += 4u)
            {
                uint32_t word = 0u;
                if (!kernel.cpu().read32(
                        source_physical + offset, &word))
                {
                    return false;
                }
                for (uint32_t b = 0u;
                     b < 4u && offset + b < size; ++b)
                {
                    transfer.m_bytes.push_back(
                        static_cast<uint8_t>(word >> (b * 8u))
                    );
                }
            }
            transfers.push_back(std::move(transfer));
        }

        // All IOP reads and EE destination ranges were checked.
        // Commit every packet and deliver the hardware SIF0 event
        // only when a guest IOP thread actually invokes sifman #7.
        for (const s_transfer& transfer : transfers)
        {
            for (size_t index = 0u; index < transfer.m_bytes.size();
                 ++index)
            {
                m_ee_ram->write8(
                    transfer.m_ee_destination +
                        static_cast<uint32_t>(index),
                    transfer.m_bytes[index]
                );
            }
        }
        if (!m_dmac->raise(5u))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = m_next_dma_id++;
        return true;
    }

    case k_get_smflag_syscall:
    {
        uint32_t value = 0u;
        if (!m_sif.get_reg(4u, &value))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = value;
        return true;
    }

    case k_set_smflag_syscall:
        // Only the IOP owns the producer side of SMFLAG. No value
        // is published until a guest R3000 thread invokes this stub.
        m_sif.publish_iop_smflag(
            kernel.cpu().state().m_gpr[4]
        );
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    default:
        return false;
    }
}
} // namespace iop
