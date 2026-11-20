#include "iop_hw.h"
#include "iop_cpu.h"

#include <algorithm>
#include <cstdio>

namespace iop
{
c_hw::c_hw()
{
    reset();
}

void c_hw::reset()
{
    std::fill(
        m_scratchpad,
        m_scratchpad + k_scratchpad_size,
        static_cast<uint8_t>(0u)
    );
    m_cdvd_i_stat = 0u;
    m_cdvd_dec_set = 0u;
    m_cdvd_ncmd_command = 0u;
    std::fill(
        m_cdvd_ncmd_params,
        m_cdvd_ncmd_params + k_cdvd_ncmd_param_capacity,
        static_cast<uint8_t>(0u)
    );
    m_cdvd_ncmd_param_count = 0u;
    m_cdvd_ncmd_pending = false;
    m_i_stat = 0u;
    m_i_mask = 0u;
    m_i_ctrl = 1u;
    m_cache_control = 0u;
    m_spu2.reset();
    m_ssbus_spu2_addr = 0u;
    m_ssbus_spu2_addr1 = 0u;
    m_ssbus_spu2_delay = 0u;
    m_ssbus_spu2_delay1 = 0u;
    m_sio2_ctrl = 0u;
    std::fill(m_sio2_send3, m_sio2_send3 + 16u, 0u);
    std::fill(m_sio2_port, m_sio2_port + 8u, 0u);
    m_sio2_pending_tx.clear();
    m_sio2_transfer_requested = false;
    m_timer5_count = 0u;
    m_timer5_mode = 0u;
    m_timer5_target = 0u;
    m_timer5_remainder = 0u;
    m_timer5_irq_armed = true;
    std::fill(m_cdrom_dma, m_cdrom_dma + 3u, 0u);
    for (auto& core : m_spu2_dma)
    {
        for (uint32_t& reg : core)
        {
            reg = 0u;
        }
    }
    m_spu2_dma_remaining[0] = 0u;
    m_spu2_dma_remaining[1] = 0u;
    m_dma_cpu = nullptr;
    m_dma_pcr = 0u;
    m_dma_pcr2 = 0u;
    for (auto& bank : m_sio2_dma)
    {
        for (uint32_t& register_value : bank)
        {
            register_value = 0u;
        }
    }
    m_dma_global_enable = 0u;
    m_sif_mscom = 0u;
    m_sif_smcom = 0u;
    m_sif_msflag = 0u;
    m_sif_smflag = 0u;
    m_sif_mmio_user = nullptr;
    m_sif0_user = nullptr;
    m_sif0_start = nullptr;
    m_sif1_user = nullptr;
    m_sif1_start = nullptr;
    m_sif_mmio_read = nullptr;
    m_sif_mmio_write = nullptr;
    m_cdvd_user = nullptr;
    m_cdvd_start = nullptr;
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            m_sif_dma[channel][reg] = 0u;
        }
    }
    m_dma_icr = 0u;
    m_dma_icr2 = 0u;
    m_debug_console.clear();
    m_debug_console_truncated = false;
}

void c_hw::bind_sif_mmio(
    void* user, t_sif_mmio_read read, t_sif_mmio_write write
)
{
    m_sif_mmio_user = (user != nullptr && read != nullptr &&
                       write != nullptr) ? user : nullptr;
    m_sif_mmio_read = m_sif_mmio_user != nullptr ? read : nullptr;
    m_sif_mmio_write = m_sif_mmio_user != nullptr ? write : nullptr;
}

void c_hw::bind_sif0_start(void* user, t_sif0_start_callback callback)
{
    m_sif0_user = callback != nullptr ? user : nullptr;
    m_sif0_start = m_sif0_user != nullptr ? callback : nullptr;
}

void c_hw::bind_sif1_start(void* user, t_sif1_start_callback callback)
{
    m_sif1_user = callback != nullptr ? user : nullptr;
    m_sif1_start = m_sif1_user != nullptr ? callback : nullptr;
}

void c_hw::bind_cdvd_start(void* user, t_cdvd_start_callback callback)
{
    m_cdvd_user = callback != nullptr ? user : nullptr;
    m_cdvd_start = m_cdvd_user != nullptr ? callback : nullptr;
}

void c_hw::bind_dma_cpu(c_cpu* cpu)
{
    m_dma_cpu = cpu;
}

bool c_hw::spu2_ram_word(uint32_t address, uint16_t* value) const
{
    return m_spu2.ram_word(address, value);
}

uint32_t c_hw::spu2_dma_cycles_remaining() const
{
    const uint32_t first = m_spu2_dma_remaining[0u];
    const uint32_t second = m_spu2_dma_remaining[1u];
    if (first == 0u)
    {
        return second;
    }
    if (second == 0u)
    {
        return first;
    }
    return std::min(first, second);
}

void c_hw::start_spu2_dma(uint32_t core)
{
    if (core >= 2u || m_dma_cpu == nullptr ||
        m_spu2_dma_remaining[core] != 0u)
    {
        return;
    }
    const uint32_t madr = m_spu2_dma[core][0u];
    const uint32_t bcr = m_spu2_dma[core][1u];
    const uint32_t chcr = m_spu2_dma[core][2u];
    const uint64_t blocks = (bcr >> 16u) != 0u ?
        (bcr >> 16u) : 1u;
    const uint64_t words = static_cast<uint64_t>(bcr & 0xffffu) * blocks;
    const uint64_t bytes = words * 4u;
    const uint32_t physical = madr & 0x1fffffffu;
    const bool to_spu = (chcr & 1u) != 0u;
    const uint32_t channel = core == 0u ? 4u : 7u;

    // Reject invalid lengths, unbacked RAM and separate AutoDMA
    // streaming rather than inventing sample bytes or a completion.
    if (words == 0u || bytes > c_cpu::k_ram_size ||
        physical > c_cpu::k_ram_size ||
        bytes > c_cpu::k_ram_size - physical ||
        (to_spu && m_spu2.auto_dma_enabled(core)))
    {
        std::printf(
            "IOP SPU2 DMA deferred: channel=%u madr=0x%08x "
            "bcr=0x%08x chcr=0x%08x auto=%u "
            "(no data or completion fabricated)\n",
            channel, madr, bcr, chcr,
            (to_spu && m_spu2.auto_dma_enabled(core)) ? 1u : 0u);
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(bytes), 0u);
    if (to_spu && !m_dma_cpu->dma_read(
            madr, data.data(), static_cast<uint32_t>(bytes)))
    {
        return;
    }
    const uint32_t prior_irq = m_spu2.irq_latches();
    if (!m_spu2.copy_dma(core, data, to_spu))
    {
        return;
    }
    if (!to_spu && !m_dma_cpu->dma_write(
            madr, data.data(), static_cast<uint32_t>(bytes)))
    {
        // The complete IOP RAM range was preflight above.
        return;
    }
    if ((m_spu2.irq_latches() & ~prior_irq) != 0u)
    {
        (void)raise_interrupt(c_spu2::k_irq_line);
    }

    // ps2-web-recomp/SDBZ completes ordinary SPU2 DMA after
    // 64 + words/4 IOP clocks. This is scheduled only once real
    // guest bytes have moved in the guest-requested direction.
    m_spu2_dma_remaining[core] =
        64u + static_cast<uint32_t>(words / 4u);
    std::printf(
        "IOP SPU2 DMA started: channel=%u madr=0x%08x "
        "bcr=0x%08x chcr=0x%08x bytes=%u delay=%u\n",
        channel, madr, bcr, chcr, static_cast<uint32_t>(bytes),
        m_spu2_dma_remaining[core]);
}

void c_hw::advance_spu2_dma(uint32_t cycles)
{
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        uint32_t& remaining = m_spu2_dma_remaining[core];
        if (remaining == 0u)
        {
            continue;
        }
        if ((m_spu2_dma[core][2u] & k_spu2_dma_str_bit) == 0u)
        {
            remaining = 0u;
            continue;
        }
        if (cycles < remaining)
        {
            remaining -= cycles;
            continue;
        }
        remaining = 0u;

        // A real sound-RAM copy completed and was not cancelled.
        // Only now may the guest see STR fall and the matching
        // DICR/DICR2 channel completion flag with shared IOP IRQ3.
        m_spu2_dma[core][2u] &= ~k_spu2_dma_str_bit;
        const uint32_t channel = core == 0u ? 4u : 7u;
        const uint32_t index = core == 0u ? 4u : 0u;
        uint32_t& control = core == 0u ? m_dma_icr : m_dma_icr2;
        control |= uint32_t{1} << (24u + index);
        assert_dma_irq(control, false);
        (void)raise_interrupt(k_dma_irq);
        std::printf(
            "IOP SPU2 DMA completed: channel=%u chcr=0x%08x "
            "dicr=0x%08x i_stat=0x%08x\n",
            channel, m_spu2_dma[core][2u], control, m_i_stat);
    }
}

bool c_hw::scratchpad_range(
    uint32_t address, uint32_t width
) const
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }
    return address >= k_scratchpad_base &&
           static_cast<uint64_t>(address - k_scratchpad_base) +
               width <= k_scratchpad_size &&
           (address & (width - 1u)) == 0u;
}

bool c_hw::sif_dma_register(uint32_t address, uint32_t* channel,
                            uint32_t* reg) const
{
    if (channel == nullptr || reg == nullptr)
    {
        return false;
    }
    if (address >= k_sif0_dma_base &&
        address < k_sif0_dma_base + 16u &&
        (address & 3u) == 0u)
    {
        *channel = 0u;
        *reg = (address - k_sif0_dma_base) / 4u;
        return true;
    }
    if (address >= k_sif1_dma_base &&
        address < k_sif1_dma_base + 16u &&
        (address & 3u) == 0u)
    {
        *channel = 1u;
        *reg = (address - k_sif1_dma_base) / 4u;
        return true;
    }
    return false;
}

bool c_hw::spu2_dma_register(
    uint32_t address, uint32_t width, uint32_t* core,
    uint32_t* reg, uint32_t* shift
) const
{
    if (core == nullptr || reg == nullptr || shift == nullptr ||
        (width != 1u && width != 2u && width != 4u) ||
        (address & (width - 1u)) != 0u)
    {
        return false;
    }
    const uint32_t bases[2] = {
        k_spu2_dma4_base, k_spu2_dma7_base
    };
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        if (address >= bases[index])
        {
            const uint32_t offset = address - bases[index];
            if (offset < k_spu2_dma_register_size &&
                width <= k_spu2_dma_register_size - offset &&
                width <= 4u - (offset & 3u))
            {
                *core = index;
                *reg = offset / 4u;
                *shift = (offset & 3u) * 8u;
                return true;
            }
        }
    }
    return false;
}

bool c_hw::read(
    uint32_t address, uint32_t width, uint32_t* value
)
{
    if (value == nullptr ||
        (width != 1u && width != 2u && width != 4u))
    {
        return false;
    }

    if (scratchpad_range(address, width))
    {
        const uint32_t offset = address - k_scratchpad_base;
        uint32_t data = 0u;
        for (uint32_t i = 0u; i < width; ++i)
        {
            data |= static_cast<uint32_t>(
                m_scratchpad[offset + i]
            ) << (8u * i);
        }
        *value = data;
        return true;
    }
    if (address >= c_spu2::k_base &&
        address < c_spu2::k_base + c_spu2::k_size)
    {
        return m_spu2.read(address, width, value);
    }
    // A physical CDVD drive with no media reports 0x00. The ROM0
    // diagnostic runner does not mount a disc. This deliberately does
    // not claim a successful CD read, device initialization, or SIF.
    // The physical controller is idle and ready to receive a command
    // after reset, independently of whether media is present.
    // Parameter and command writes are accepted; no disc producer
    // is attached, so commands remain pending without a fake IRQ.
    // CDVD interrupt reasons are distinct from the IOP I_STAT register.
    // Firmware can acknowledge only bits previously raised by genuine
    // CDVD hardware; this model has no CDVD event producer yet.
    if (address == k_cdvd_i_stat && width == 1u)
    {
        *value = m_cdvd_i_stat;
        return true;
    }
    if (address == k_cdvd_dec_set && width == 1u)
    {
        *value = m_cdvd_dec_set;
        return true;
    }
    if (address == k_cdvd_ncmd_command && width == 1u)
    {
        *value = m_cdvd_ncmd_command;
        return true;
    }
    if (address == k_cdvd_ncmd_status && width == 1u)
    {
        *value = m_cdvd_ncmd_pending ?
            k_cdvd_ncmd_busy : k_cdvd_ncmd_ready;
        return true;
    }
    if (address == k_cdvd_disc_type && width == 1u)
    {
        *value = k_cdvd_no_disc;
        return true;
    }
    // No guest controller or memory-card producer is installed.
    // An empty receive FIFO cannot supply fictitious reply bytes.
    if (address == k_sio2_rx_fifo)
    {
        return false;
    }

    // The actual LIBSD DMA4.BCR path uses SH, and the
    // ps2-web-recomp/SDBZ hardware model extracts subword lanes.
    // Only these two known DMA4/DMA7 register banks are relaxed.
    uint32_t spu2_core = 0u;
    uint32_t spu2_reg = 0u;
    uint32_t spu2_shift = 0u;
    if (spu2_dma_register(
            address, width, &spu2_core, &spu2_reg, &spu2_shift))
    {
        const uint32_t mask = width == 4u ?
            0xffffffffu : ((uint32_t{1} << (width * 8u)) - 1u);
        *value = (m_spu2_dma[spu2_core][spu2_reg] >>
                  spu2_shift) & mask;
        return true;
    }

    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }
    if (address >= k_sio2_send3_base &&
        address < k_sio2_send3_base + 0x40u)
    {
        *value = m_sio2_send3[(address - k_sio2_send3_base) / 4u];
        return true;
    }
    if (address >= k_sio2_port_base &&
        address < k_sio2_port_base + 0x20u)
    {
        *value = m_sio2_port[(address - k_sio2_port_base) / 4u];
        return true;
    }

    if (address >= k_cdrom_dma_base &&
        address < k_cdrom_dma_base + 12u)
    {
        *value = m_cdrom_dma[(address - k_cdrom_dma_base) / 4u];
        return true;
    }


    for (uint32_t index = 0u; index < 2u; ++index)
    {
        const uint32_t base = index == 0u
            ? k_sio2_dma_rx_base : k_sio2_dma_tx_base;
        if (address >= base && address < base + 12u)
        {
            *value = m_sio2_dma[index][(address - base) / 4u];
            return true;
        }
    }

    uint32_t channel = 0u;
    uint32_t reg = 0u;
    if (sif_dma_register(address, &channel, &reg))
    {
        *value = m_sif_dma[channel][reg];
        return true;
    }

    if (m_sif_mmio_user != nullptr &&
        (address == k_sif_mscom || address == k_sif_smcom ||
         address == k_sif_msflag || address == k_sif_smflag))
    {
        return m_sif_mmio_read(m_sif_mmio_user, address, width, value);
    }
    switch (address)
    {
    case k_ssbus_spu2_addr:
        *value = m_ssbus_spu2_addr;
        return true;
    case k_ssbus_spu2_addr1:
        *value = m_ssbus_spu2_addr1;
        return true;
    case k_ssbus_spu2_delay:
        *value = m_ssbus_spu2_delay;
        return true;
    case k_ssbus_spu2_delay1:
        *value = m_ssbus_spu2_delay1;
        return true;
    case k_sif_mscom:
        *value = m_sif_mscom;
        return true;
    case k_sif_smcom:
        *value = m_sif_smcom;
        return true;
    case k_sif_msflag:
        *value = m_sif_msflag;
        return true;
    case k_sif_smflag:
        *value = m_sif_smflag;
        return true;
    case k_dma_pcr:
        *value = m_dma_pcr;
        return true;
    case k_dma_pcr2:
        *value = m_dma_pcr2;
        return true;
    case k_dma_global_enable:
        *value = m_dma_global_enable;
        return true;
    case k_dma_icr:
        *value = m_dma_icr;
        return true;
    case k_dma_icr2:
        *value = m_dma_icr2;
        return true;
    case k_timer5_count:
        *value = m_timer5_count;
        return true;
    case k_timer5_mode:
        *value = m_timer5_mode | (m_timer5_irq_armed ? 0x400u : 0u);
        // Counter target/overflow status bits are cleared on read.
        m_timer5_mode &= ~0x1800u;
        return true;
    case k_timer5_target:
        *value = m_timer5_target;
        return true;
    case k_i_stat:
        *value = m_i_stat;
        return true;
    case k_i_mask:
        *value = m_i_mask;
        return true;
    case k_i_ctrl:
        // On the IOP, reading I_CTRL disables the global interrupt
        // enable until the guest writes it back.
        *value = m_i_ctrl;
        m_i_ctrl = 0u;
        return true;
    case k_cache_control:
        // Register storage only. The emulator does not yet model
        // I-cache/D-cache isolation or instruction fetch coherence.
        *value = m_cache_control;
        return true;
    case k_sio2_ctrl:
        *value = m_sio2_ctrl;
        return true;
    case k_sio2_stat:
        // No SIO2 transaction has completed.
        *value = 0u;
        return true;
    case k_sio2_port_status:
        *value = 0x0fu; // Port register mask, not pad readiness.
        return true;
    case k_sio2_aux_status:
        *value = 0u;
        return true;
    default:
        return false;
    }
}

bool c_hw::write(
    uint32_t address, uint32_t width, uint32_t value
)
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }
    if (scratchpad_range(address, width))
    {
        const uint32_t offset = address - k_scratchpad_base;
        for (uint32_t i = 0u; i < width; ++i)
        {
            m_scratchpad[offset + i] =
                static_cast<uint8_t>(value >> (8u * i));
        }
        return true;
    }
    if (address >= c_spu2::k_base &&
        address < c_spu2::k_base + c_spu2::k_size)
    {
        const uint32_t prior_irq = m_spu2.irq_latches();
        if (!m_spu2.write(address, width, value))
        {
            return false;
        }
        // Only a real data-port access to the guest-selected IRQA
        // can assert IRQ9. The SPU2 IRQ latch is edge-triggered here:
        // an unrelated register write must not reassert an IRQ the
        // guest already acknowledged at I_STAT.
        if ((m_spu2.irq_latches() & ~prior_irq) != 0u)
        {
            (void)raise_interrupt(c_spu2::k_irq_line);
        }
        return true;
    }
    // OpenPS2BIOS writes individual characters here. This address is
    // a debug-output convention in emulators, not a PS2 readiness flag.
    // Accept only the observed byte-store operation; fail other MMIO.
    if (address == k_cdvd_i_stat && width == 1u)
    {
        // CDVD I_STAT is W1C. An ack does not generate an IOP IRQ
        // or mark any CDVD operation as completed.
        m_cdvd_i_stat &= static_cast<uint8_t>(~value);
        return true;
    }
    if (address == k_cdvd_dec_set && width == 1u)
    {
        // Firmware configures the CDVD decryption mode here. Retain
        // the byte until reset; no sector processing is implied.
        m_cdvd_dec_set = static_cast<uint8_t>(value);
        return true;
    }
    if (address == k_cdvd_ncmd_status && width == 1u)
    {
        // This physical address is N_READY when read, N_DATAIN when
        // written. Keep the original ordered parameters for CDVD.
        if (m_cdvd_ncmd_pending ||
            m_cdvd_ncmd_param_count >= k_cdvd_ncmd_param_capacity)
        {
            return false;
        }
        m_cdvd_ncmd_params[m_cdvd_ncmd_param_count++] =
            static_cast<uint8_t>(value);
        return true;
    }
    if (address == k_cdvd_ncmd_command && width == 1u)
    {
        if (m_cdvd_ncmd_pending)
        {
            return false;
        }
        m_cdvd_ncmd_command = static_cast<uint8_t>(value);
        m_cdvd_ncmd_pending = true;
        // An actual sector provider must consume/complete this request.
        // Do not supply empty sector data or clear DMA3 STR here.
        if (m_cdvd_start != nullptr)
        {
            m_cdvd_start(m_cdvd_user);
        }
        return true;
    }
    if (address == k_debug_console && width == 1u)
    {
        if (m_debug_console.size() < k_debug_console_capacity)
        {
            m_debug_console.push_back(static_cast<char>(value & 0xffu));
        }
        else
        {
            m_debug_console_truncated = true;
        }
        return true;
    }
    // Guest FIFO transmit requests are queued, never executed without
    // a real SIO2 producer; no DMA completion or IRQ follows.
    if (address == k_sio2_tx_fifo &&
        (width == 1u || width == 4u))
    {
        if (m_sio2_pending_tx.size() >= k_sio2_tx_limit)
        {
            return false;
        }
        m_sio2_pending_tx.push_back(static_cast<uint8_t>(value));
        return true;
    }

    uint32_t spu2_core = 0u;
    uint32_t spu2_reg = 0u;
    uint32_t spu2_shift = 0u;
    if (spu2_dma_register(
            address, width, &spu2_core, &spu2_reg, &spu2_shift))
    {
        // Retain the untouched bits of MADR, BCR, CHCR or TADR.
        // This is the primary reference's little-endian subword
        // merge, including 0xBF8010C4 and its upper half at 0xC6.
        const uint32_t lane_mask = width == 4u ?
            0xffffffffu : ((uint32_t{1} << (width * 8u)) - 1u);
        const uint32_t mask = lane_mask << spu2_shift;
        uint32_t& word = m_spu2_dma[spu2_core][spu2_reg];
        const uint32_t previous = word;
        word = (word & ~mask) | ((value << spu2_shift) & mask);
        if (spu2_reg == 2u && (mask & k_spu2_dma_str_bit) != 0u)
        {
            if ((word & k_spu2_dma_str_bit) == 0u)
            {
                // Driver cancellation must never complete a stopped
                // DMA or set a phantom DICR interrupt flag.
                m_spu2_dma_remaining[spu2_core] = 0u;
            }
            else if ((previous & k_spu2_dma_str_bit) == 0u)
            {
                start_spu2_dma(spu2_core);
            }
        }
        return true;
    }

    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }
    if (address >= k_sio2_send3_base &&
        address < k_sio2_send3_base + 0x40u)
    {
        m_sio2_send3[(address - k_sio2_send3_base) / 4u] = value;
        return true;
    }
    if (address >= k_sio2_port_base &&
        address < k_sio2_port_base + 0x20u)
    {
        m_sio2_port[(address - k_sio2_port_base) / 4u] = value;
        return true;
    }

    if (address >= k_cdrom_dma_base &&
        address < k_cdrom_dma_base + 12u)
    {
        const uint32_t reg = (address - k_cdrom_dma_base) / 4u;
        // Programming CHCR.STR requests CD-ROM DMA3. In the
        // absence of a disc/data producer, retain the requested
        // state (including STR) without transferring bytes, clearing
        // STR, asserting an IRQ or claiming command completion.
        // A future disc transport must explicitly complete it.
        m_cdrom_dma[reg] = value;
        if (reg == 2u && (value & 0x01000000u) != 0u &&
            m_cdvd_start != nullptr)
        {
            m_cdvd_start(m_cdvd_user);
        }
        return true;
    }

    for (uint32_t index = 0u; index < 2u; ++index)
    {
        const uint32_t base = index == 0u
            ? k_sio2_dma_rx_base : k_sio2_dma_tx_base;
        if (address >= base && address < base + 12u)
        {
            // No SIO2 DMA data producer is connected. The transfer
            // remains requested, and no hardware IRQ/completion occurs.
            m_sio2_dma[index][(address - base) / 4u] = value;
            return true;
        }
    }

    uint32_t channel = 0u;
    uint32_t reg = 0u;
    if (sif_dma_register(address, &channel, &reg))
    {
        // CHCR.STR is guest-controlled when starting/stopping DMA, and
        // is NOT cleared merely because the register write succeeded.
        // The transfer backend will clear STR on real completion.
        const bool was_started =
            (m_sif_dma[channel][2u] & 0x01000000u) != 0u;
        m_sif_dma[channel][reg] = value;
        if (reg == 2u && !was_started &&
            (value & 0x01000000u) != 0u)
        {
            if (channel == 0u && m_sif0_start != nullptr)
            {
                m_sif0_start(m_sif0_user);
            }
            else if (channel == 1u && m_sif1_start != nullptr)
            {
                m_sif1_start(m_sif1_user);
            }
        }
        return true;
    }

    if (m_sif_mmio_user != nullptr &&
        (address == k_sif_mscom || address == k_sif_smcom ||
         address == k_sif_msflag || address == k_sif_smflag))
    {
        return m_sif_mmio_write(m_sif_mmio_user, address, width, value);
    }
    switch (address)
    {
    case k_ssbus_spu2_addr:
        m_ssbus_spu2_addr = value;
        return true;
    case k_ssbus_spu2_addr1:
        m_ssbus_spu2_addr1 = value;
        return true;
    case k_ssbus_spu2_delay:
        m_ssbus_spu2_delay = value;
        return true;
    case k_ssbus_spu2_delay1:
        m_ssbus_spu2_delay1 = value;
        return true;
    case k_sif_smcom:
        m_sif_smcom = value;
        return true;
    case k_sif_msflag:
        // IOP writes acknowledge bits asserted by the EE.
        m_sif_msflag &= ~value;
        return true;
    case k_sif_smflag:
        // Only the guest IOP announces its own readiness. The EE
        // independently acknowledges flags when it processes them.
        m_sif_smflag |= value;
        return true;
    case k_dma_pcr:
        // Each DMA channel's priority and enable nibble is writable,
        // but programming it does not launch or complete a transfer.
        m_dma_pcr = value;
        return true;
    case k_dma_pcr2:
        m_dma_pcr2 = value;
        return true;
    case k_dma_global_enable:
        // This permits an explicitly started secondary DMA controller,
        // but never asserts a DMA completion or a SIF handshake.
        m_dma_global_enable = value;
        return true;
    case k_dma_icr:
        m_dma_icr = update_dma_icr(m_dma_icr, value);
        return true;
    case k_dma_icr2:
        m_dma_icr2 = update_dma_icr(m_dma_icr2, value);
        return true;
    case k_timer5_count:
        m_timer5_count = value;
        m_timer5_remainder = 0u;
        return true;
    case k_timer5_target:
        m_timer5_target = value;
        return true;
    case k_timer5_mode:
        // Only accept the supported counter-clock, target-reset/IRQ,
        // repeat, non-toggle mode, plus the disabled interrupt mode.
        // Unknown gate/source/overflow configurations must not run
        // with invented counter behaviour.
        if ((value & ~0x6058u) != 0u ||
            (value & 0x0058u) != 0x0058u)
        {
            // Also permit mode 0 to reset an unconfigured counter.
            if (value != 0u)
            {
                return false;
            }
        }
        m_timer5_mode = value & 0x6058u;
        m_timer5_count = 0u;
        m_timer5_remainder = 0u;
        m_timer5_irq_armed = true;
        return true;
    case k_i_stat:
        // Write-one keeps the IRQ; write-zero acknowledges it.
        m_i_stat &= value;
        return true;
    case k_i_mask:
        m_i_mask = value;
        return true;
    case k_i_ctrl:
        m_i_ctrl = value & 1u;
        return true;
    case k_cache_control:
        m_cache_control = value;
        return true;
    case k_sio2_ctrl:
        m_sio2_ctrl = value;
        if ((value & 0x0cu) == 0x0cu)
        {
            m_sio2_pending_tx.clear();
            m_sio2_transfer_requested = false;
        }
        if ((value & 1u) != 0u)
        {
            m_sio2_transfer_requested = true;
        }
        // No device bytes, completion, or interrupt are manufactured.
        return true;
    default:
        return false;
    }
}

void c_hw::ee_write_mscom(uint32_t value)
{
    m_sif_mscom = value;
}

void c_hw::ee_set_msflag(uint32_t bits)
{
    m_sif_msflag |= bits;
}

void c_hw::ee_clear_smflag(uint32_t bits)
{
    m_sif_smflag &= ~bits;
}

uint32_t c_hw::sif_smcom() const
{
    uint32_t value = m_sif_smcom;
    if (m_sif_mmio_user != nullptr)
    {
        (void)m_sif_mmio_read(
            m_sif_mmio_user, k_sif_smcom, 4u, &value
        );
    }
    return value;
}

uint32_t c_hw::sif_smflag() const
{
    uint32_t value = m_sif_smflag;
    if (m_sif_mmio_user != nullptr)
    {
        (void)m_sif_mmio_read(
            m_sif_mmio_user, k_sif_smflag, 4u, &value
        );
    }
    return value;
}

void c_hw::assert_dma_irq(uint32_t& control, bool sif_channel)
{
    const bool force = (control & 0x00008000u) != 0u;
    const bool enabled = (control & 0x00800000u) != 0u;
    const bool channel_pending =
        (((control >> 8u) & control) & 0x007f0000u) != 0u;
    if (force || (enabled && channel_pending))
    {
        control |= 0x80000000u;
    }
    else
    {
        control &= ~0x80000000u;
    }
    if (sif_channel || (control & 0x80000000u) != 0u)
    {
        (void)raise_interrupt(k_dma_irq);
    }
}

uint32_t c_hw::update_dma_icr(uint32_t previous, uint32_t value)
{
    // Low 24 bits are writable control; pending bits 24..30 are
    // hardware flags acknowledged by write-one-to-clear. Bit 31
    // reflects the combined force/master/channel interrupt state.
    uint32_t result =
        (previous & 0x7f000000u) | (value & 0x00ffffffu);
    result &= ~(value & 0x7f000000u);
    assert_dma_irq(result, false);
    return result;
}

bool c_hw::complete_dma_channel(uint32_t channel)
{
    if (channel > 12u)
    {
        return false;
    }
    const bool secondary = channel >= 7u;
    const uint32_t index = secondary ? channel - 7u : channel;
    if (channel == 9u || channel == 10u)
    {
        // CHCR.STR clears only at a real completion supplied by the
        // DMA engine. Do not move bytes or synthesize a transfer here.
        m_sif_dma[channel - 9u][2u] &= ~0x01000000u;
    }
    uint32_t& control = secondary ? m_dma_icr2 : m_dma_icr;
    const bool sif_channel = secondary && (index == 2u || index == 3u);
    const uint32_t enable = uint32_t{1} << (16u + index);
    // PCSX2 models SIF0/SIF1 end/tag interrupts independently of
    // the per-channel mask; they do not fabricate any DMA completion.
    if ((control & enable) != 0u || sif_channel)
    {
        control |= uint32_t{1} << (24u + index);
        assert_dma_irq(control, sif_channel);
    }
    return true;
}

bool c_hw::raise_interrupt(uint32_t line)
{
    if (line >= 32u)
    {
        return false;
    }
    m_i_stat |= uint32_t{1} << line;
    return true;
}

bool c_hw::supports_interrupt_line(uint32_t line)
{
    return line < 39u || (line >= 40u && line < 46u);
}

bool c_hw::set_interrupt_mask(uint32_t line, bool enabled)
{
    if (!supports_interrupt_line(line))
    {
        return false;
    }
    if (line < 32u)
    {
        const uint32_t bit = uint32_t{1} << line;
        if (enabled)
        {
            m_i_mask |= bit;
        }
        else
        {
            m_i_mask &= ~bit;
        }
        return true;
    }

    // Primary channels 0..6 are INTRMAN lines 32..38. Secondary
    // channels 7..12 are lines 40..45; line 39 is not a channel.
    // Match ps2-web-recomp/SDBZ hw::set_interrupt_mask().
    const bool secondary = line >= 40u;
    uint32_t& control = secondary ? m_dma_icr2 : m_dma_icr;
    const uint32_t channel = secondary ? line - 40u : line - 32u;
    const uint32_t bit = uint32_t{1} << (16u + channel);
    if (enabled)
    {
        // DMA IRQs arrive through the ordinary IOP IRQ3 aggregator.
        // No new request or interrupt is generated by this enable.
        m_i_mask |= uint32_t{1} << k_dma_irq;
        m_dma_icr |= 0x00800000u;
        control |= bit;
    }
    else
    {
        // Disabling a channel does not acknowledge pending DMA flags,
        // clear the shared IRQ3 mask or stop a running DMA transfer.
        control &= ~bit;
    }
    assert_dma_irq(control, false);
    return true;
}

uint32_t c_hw::pending_interrupts() const
{
    return m_i_ctrl != 0u ? (m_i_stat & m_i_mask) : 0u;
}

uint32_t c_hw::cache_control() const
{
    return m_cache_control;
}

void c_hw::advance_cycles(uint32_t cycles)
{
    if (cycles == 0u)
    {
        return;
    }
    advance_spu2_dma(cycles);
    // Timer 5 clock division, per IOP counter MODE[14:13].
    // The initial kernel tick uses /1, mode 0x58.
    const uint32_t shifts[4] = {1u, 8u, 16u, 256u};
    const uint32_t divider = shifts[(m_timer5_mode >> 13u) & 3u];
    const uint64_t accumulated =
        static_cast<uint64_t>(m_timer5_remainder) + cycles;
    const uint64_t elapsed = accumulated / divider;
    m_timer5_remainder = static_cast<uint32_t>(accumulated % divider);
    if (elapsed == 0u)
    {
        return;
    }

    const uint64_t count = static_cast<uint64_t>(m_timer5_count) + elapsed;
    if ((m_timer5_mode & 0x08u) != 0u &&
        m_timer5_target != 0u &&
        m_timer5_count < m_timer5_target &&
        count >= m_timer5_target)
    {
        const uint64_t over = count - m_timer5_target;
        m_timer5_count = static_cast<uint32_t>(
            over % m_timer5_target
        );
        m_timer5_mode |= 0x0800u;
        if ((m_timer5_mode & 0x10u) != 0u && m_timer5_irq_armed)
        {
            (void)raise_interrupt(k_timer5_irq);
            if ((m_timer5_mode & 0x40u) == 0u)
            {
                m_timer5_irq_armed = false;
            }
        }
    }
    else
    {
        // Unconfigured/free-running timer, or a counter past its
        // target: normal 32-bit rollover without an invented IRQ.
        m_timer5_count = static_cast<uint32_t>(count);
    }
}

bool c_hw::cdvd_ncmd_pending() const
{
    return m_cdvd_ncmd_pending;
}

uint8_t c_hw::cdvd_ncmd_command() const
{
    return m_cdvd_ncmd_command;
}

uint32_t c_hw::cdvd_ncmd_parameter_count() const
{
    return m_cdvd_ncmd_param_count;
}

bool c_hw::cdvd_ncmd_parameter(uint32_t index, uint8_t* value) const
{
    if (value == nullptr || index >= m_cdvd_ncmd_param_count)
    {
        return false;
    }
    *value = m_cdvd_ncmd_params[index];
    return true;
}

bool c_hw::cdrom_dma_register(uint32_t index, uint32_t* value) const
{
    if (value == nullptr || index >= 3u)
    {
        return false;
    }
    *value = m_cdrom_dma[index];
    return true;
}

size_t c_hw::sio2_pending_tx_bytes() const
{
    return m_sio2_pending_tx.size();
}

bool c_hw::sio2_tx_byte(size_t index, uint8_t* value) const
{
    if (value == nullptr || index >= m_sio2_pending_tx.size())
    {
        return false;
    }
    *value = m_sio2_pending_tx[index];
    return true;
}

bool c_hw::sio2_transfer_requested() const
{
    return m_sio2_transfer_requested;
}

bool c_hw::complete_cdvd_read(uint8_t interrupt_reason)
{
    if (!m_cdvd_ncmd_pending || m_cdvd_ncmd_command != 0x06u ||
        (m_cdrom_dma[2u] & 0x01000000u) == 0u)
    {
        return false;
    }
    m_cdrom_dma[2u] &= ~0x01000000u;
    m_cdvd_ncmd_pending = false;
    m_cdvd_ncmd_param_count = 0u;
    m_cdvd_i_stat |= interrupt_reason;
    (void)complete_dma_channel(3u);
    (void)raise_interrupt(2u);
    return true;
}

const std::string& c_hw::debug_console() const
{
    return m_debug_console;
}

bool c_hw::debug_console_truncated() const
{
    return m_debug_console_truncated;
}
} // namespace iop
