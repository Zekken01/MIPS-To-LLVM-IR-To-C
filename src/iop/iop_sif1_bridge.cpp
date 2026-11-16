#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"

#include <utility>
#include <vector>

namespace iop
{
namespace
{
bool normalize_ee_address(uint32_t guest_address, uint32_t* physical)
{
    if (physical == nullptr)
    {
        return false;
    }

    if (guest_address >= 0x80000000u &&
        guest_address < 0xc0000000u)
    {
        *physical = guest_address & 0x1fffffffu;
        return true;
    }

    const uint32_t region = guest_address >> 28u;
    if (region == 2u || region == 3u)
    {
        *physical = guest_address & 0x0fffffffu;
        return true;
    }

    // The EE scratchpad is a legitimate SIF source. Other MMIO and
    // TLB-mapped regions are not assumed to be ordinary memory.
    if (guest_address < 0x02000000u ||
        (guest_address >= ee::ee_memory::k_scratchpad_base &&
         guest_address - ee::ee_memory::k_scratchpad_base <
             ee::ee_memory::k_scratchpad_size))
    {
        *physical = guest_address;
        return true;
    }
    return false;
}

bool normalize_iop_address(uint32_t guest_address, uint32_t* physical)
{
    if (physical == nullptr)
    {
        return false;
    }

    const uint32_t region = guest_address & 0xe0000000u;
    if (region != 0u &&
        region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    *physical = guest_address & 0x1fffffffu;
    return true;
}

struct s_transfer
{
    uint32_t m_destination = 0u;
    std::vector<uint32_t> m_words;
    bool m_command = false;
    c_sifcmd_bridge::s_command m_command_effect{};
};
} // namespace

c_sif1_bridge::c_sif1_bridge(
    ee::ee_memory& ee_ram, c_cpu& iop_cpu, ee::c_ee_dmac& dmac,
    c_sifcmd_bridge* sifcmd
)
    : m_ee_ram(ee_ram), m_iop_cpu(iop_cpu), m_dmac(dmac),
      m_sifcmd(sifcmd)
{
}

bool c_sif1_bridge::set_dma(
    uint32_t list, uint32_t count, uint32_t* dma_id
)
{
    uint32_t list_physical = 0u;
    if (dma_id == nullptr ||
        m_next_dma_id == 0u ||
        count == 0u || count > 64u ||
        (list & 0x0fu) != 0u ||
        !normalize_ee_address(list, &list_physical) ||
        !m_ee_ram.contains(list_physical, count * 16u))
    {
        return false;
    }

    std::vector<s_transfer> transfers;
    transfers.reserve(count);
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint32_t descriptor = list_physical + index * 16u;
        const uint32_t source = m_ee_ram.read32(descriptor);
        const uint32_t destination = m_ee_ram.read32(descriptor + 4u);
        const uint32_t size = m_ee_ram.read32(descriptor + 8u);
        const uint32_t attributes = m_ee_ram.read32(descriptor + 12u);

        uint32_t ee_source = 0u;
        uint32_t iop_destination = 0u;
        const uint64_t rounded =
            (static_cast<uint64_t>(size) + 15u) & ~uint64_t(15u);

        // Destination zero requests an IOP reboot, and special RPC
        // mailboxes require command processing. Until those exist, do
        // not silently turn either operation into an ordinary RAM copy.
        if (size == 0u || size > 65536u ||
            attributes != 0u ||
            (source & 0x0fu) != 0u ||
            (destination & 0x0fu) != 0u ||
            !normalize_ee_address(source, &ee_source) ||
            !normalize_iop_address(destination, &iop_destination) ||
            iop_destination == 0u ||
            static_cast<uint64_t>(iop_destination) + rounded >
                c_cpu::k_ram_size ||
            // The reserved SIFCMD receiving region must not be
            // overwritten by arbitrary overlapping DMA transfers.
            (static_cast<uint64_t>(iop_destination) <
                 c_sifcmd_bridge::k_command_buffer + 0x400u &&
             static_cast<uint64_t>(iop_destination) + rounded >
                 c_sifcmd_bridge::k_command_buffer &&
             iop_destination != c_sifcmd_bridge::k_command_buffer) ||
            !m_ee_ram.contains(ee_source, static_cast<size_t>(rounded)))
        {
            return false;
        }

        s_transfer transfer{};
        transfer.m_destination = iop_destination;
        transfer.m_words.reserve(static_cast<size_t>(rounded / 4u));
        for (uint32_t offset = 0u;
             offset < rounded; offset += 4u)
        {
            transfer.m_words.push_back(
                m_ee_ram.read32(ee_source + offset)
            );
        }
        if (iop_destination == c_sifcmd_bridge::k_command_buffer)
        {
            // Only guest-initialized, implemented system commands may
            // pass through the reserved IOP command receiver.
            if (m_sifcmd == nullptr ||
                !m_sifcmd->prepare_command(
                    transfer.m_words, size, &transfer.m_command_effect))
            {
                return false;
            }
            transfer.m_command = true;
        }
        transfers.push_back(std::move(transfer));
    }

    // No writes occur until all sources and IOP destinations are valid.
    // The R3000 RAM writes below cannot fail after those checks.
    for (const s_transfer& transfer : transfers)
    {
        for (size_t index = 0u; index < transfer.m_words.size(); ++index)
        {
            m_iop_cpu.write32(
                transfer.m_destination +
                    static_cast<uint32_t>(index * 4u),
                transfer.m_words[index]
            );
        }
    }

    // Command effects are committed only once every DMA write has
    // completed. There are no invented SIF replies or readiness bits.
    for (const s_transfer& transfer : transfers)
    {
        if (transfer.m_command)
        {
            m_sifcmd->commit_command(transfer.m_command_effect);
        }
    }

    m_dmac.raise(6u);
    *dma_id = m_next_dma_id++;
    return true;
}

bool c_sif1_bridge::dma_stat(
    uint32_t dma_id, int32_t* result
) const
{
    if (result == nullptr || dma_id == 0u ||
        dma_id >= m_next_dma_id)
    {
        return false;
    }

    // This transport completes synchronously. No queued or in-flight
    // transfer is reported without a real EE/IOP cycle scheduler.
    *result = -1;
    return true;
}
} // namespace iop
