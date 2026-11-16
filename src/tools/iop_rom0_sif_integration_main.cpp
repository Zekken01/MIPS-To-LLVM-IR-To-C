#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "iop_kernel.h"
#include "iop_sif_mmio_link.h"
#include "iop_sif0_dma_transport.h"
#include "iop_sif1_dma_transport.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace
{
constexpr uint32_t k_ee_tag = 0x00100000u;
constexpr uint32_t k_payload_bytes = 16u;
constexpr uint64_t k_boot_instructions = 10000000u;

const char* stop_name(iop::e_kernel_stop stop)
{
    switch (stop)
    {
    case iop::e_kernel_stop::budget_exhausted: return "BUDGET_EXHAUSTED";
    case iop::e_kernel_stop::idle: return "IDLE";
    case iop::e_kernel_stop::unhandled_syscall: return "UNHANDLED_SYSCALL";
    case iop::e_kernel_stop::unsupported_instruction: return "UNSUPPORTED_INSTRUCTION";
    case iop::e_kernel_stop::memory_fault: return "MEMORY_FAULT";
    case iop::e_kernel_stop::arithmetic_overflow: return "ARITHMETIC_OVERFLOW";
    case iop::e_kernel_stop::breakpoint: return "BREAKPOINT";
    default: return "UNKNOWN";
    }
}

bool write_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 word{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_sif1_base + offset, 4u, &word);
}

bool read_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t* value)
{
    ee::s_ee_u128 word{};
    const bool valid = value != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &dmac, ee::c_ee_dmac::k_sif1_base + offset, 4u, &word);
    if (valid)
    {
        *value = static_cast<uint32_t>(word.m_lo);
    }
    return valid;
}

uint32_t byte_sum(uint32_t word)
{
    return (word & 0xffu) + ((word >> 8u) & 0xffu) +
           ((word >> 16u) & 0xffu) + ((word >> 24u) & 0xffu);
}

int failed(const char* reason, const iop::c_kernel& kernel)
{
    std::fprintf(
        stderr, "OpenPS2BIOS genuine SIF1 integration: %s "
        "(guest_pc=0x%08x, instructions=%llu)\n",
        reason, kernel.cpu().state().m_pc,
        static_cast<unsigned long long>(kernel.cycles()));
    return 1;
}
} // namespace

// Offsets are from the pinned OpenPS2BIOS v0.6 guest's
// include/openbios/sif.h s_ob_sif_status layout. They are only
// diagnostic reads: no host code alters the guest service state.
constexpr uint32_t k_firmware_sif_status = 0x00045000u;
constexpr uint32_t k_firmware_status_magic = 0x4f425346u;
constexpr uint32_t k_firmware_status_received = k_firmware_sif_status + 4u;
constexpr uint32_t k_ee_sifcmd_reply = 0x00180000u;

bool run_until_firmware_receives(
    iop::c_kernel& kernel, uint32_t baseline,
    uint32_t* observed)
{
    for (uint32_t attempt = 0u; attempt < 128u; ++attempt)
    {
        uint32_t count = 0u;
        if (!kernel.cpu().read32(
                k_firmware_status_received, &count))
        {
            return false;
        }
        if (count > baseline)
        {
            if (observed != nullptr)
            {
                *observed = count;
            }
            return true;
        }
        const iop::s_kernel_run_result executed =
            kernel.run(4096u, 256u);
        if (executed.m_stop !=
                iop::e_kernel_stop::budget_exhausted)
        {
            return false;
        }
    }
    return false;
}

bool wait_firmware_receiver(iop::c_kernel& kernel)
{
    for (uint32_t attempt = 0u; attempt < 128u; ++attempt)
    {
        uint32_t chcr = 0u;
        if (!kernel.hardware().read(
                iop::c_hw::k_sif1_dma_base + 8u,
                4u, &chcr))
        {
            return false;
        }
        if ((chcr & 0x01000000u) != 0u)
        {
            return true;
        }
        const iop::s_kernel_run_result executed =
            kernel.run(256u, 64u);
        if (executed.m_stop !=
                iop::e_kernel_stop::budget_exhausted)
        {
            return false;
        }
    }
    return false;
}

bool send_firmware_sifcmd(
    ee::ee_memory& memory, ee::c_ee_dmac& dmac,
    iop::c_sif1_dma_transport& transport,
    iop::c_kernel& kernel, uint32_t destination,
    const uint32_t* words, uint32_t count)
{
    if (words == nullptr || count < 4u || count > 28u ||
        !wait_firmware_receiver(kernel))
    {
        return false;
    }
    const uint32_t packet_bytes = count * 4u;
    const uint32_t padded_bytes = (packet_bytes + 15u) & ~15u;
    const uint32_t source_qwc = 1u + padded_bytes / 16u;
    const uint64_t delivered_before = transport.delivered_bytes();
    memory.write32(k_ee_tag, 0x70000000u | source_qwc);
    memory.write32(k_ee_tag + 4u, 0u);
    memory.write32(k_ee_tag + 8u, 0u);
    memory.write32(k_ee_tag + 12u, 0u);
    memory.write32(k_ee_tag + 16u, 0x40000000u | destination);
    memory.write32(k_ee_tag + 20u, padded_bytes);
    memory.write32(k_ee_tag + 24u, 0u);
    memory.write32(k_ee_tag + 28u, 0u);
    for (uint32_t index = 0u; index < padded_bytes / 4u; ++index)
    {
        memory.write32(
            k_ee_tag + 32u + index * 4u,
            index < count ? words[index] : 0u);
    }
    if (!write_ee(dmac, 0x20u, 0u) ||
        !write_ee(dmac, 0x30u, k_ee_tag) ||
        !write_ee(dmac, 0u, 0x105u) ||
        transport.faulted() ||
        transport.delivered_bytes() != delivered_before + padded_bytes)
    {
        return false;
    }
    return true;
}

bool arm_firmware_sif0_receiver(ee::c_ee_dmac& dmac)
{
    const ee::s_ee_u128 zero{0u, 0u};
    const ee::s_ee_u128 start{0x184u, 0u};
    return ee::c_ee_dmac::mmio_write(
               &dmac, ee::c_ee_dmac::k_sif0_base + 0x20u,
               4u, &zero) &&
           ee::c_ee_dmac::mmio_write(
               &dmac, ee::c_ee_dmac::k_sif0_base,
               4u, &start);
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr,
            "usage: %s <verified-OpenPS2BIOS-ROM0.bin> <report.json>\n",
            argv[0]);
        return 2;
    }

    std::ifstream rom_file(argv[1], std::ios::binary | std::ios::ate);
    if (!rom_file ||
        rom_file.tellg() !=
            static_cast<std::streampos>(iop::c_cpu::k_rom0_size))
    {
        std::fprintf(stderr, "ROM0 is missing or not exactly 4 MiB\n");
        return 2;
    }
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size);
    rom_file.seekg(0, std::ios::beg);
    if (!rom_file.read(reinterpret_cast<char*>(rom.data()),
                       static_cast<std::streamsize>(rom.size())))
    {
        std::fprintf(stderr, "ROM0 could not be read\n");
        return 2;
    }

    iop::c_kernel kernel{};
    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_hw_bus ee_hardware{};
    ee_hardware.bind_cpu(&ee_cpu);
    ee_hardware.bind_memory(&memory);
    ee_hardware.reset();

    if (!kernel.start_rom0_boot(rom))
    {
        return failed("guest reset refused", kernel);
    }
    const uint32_t reset_pc = kernel.cpu().state().m_pc;
    // This is the actual OpenPS2BIOS ROM0 guest, not the test's
    // synthetic R3000 program or the IOP IRX/HLE compatibility path.
    iop::c_sif_mmio_link::bind(
        kernel.hardware(), ee_hardware.sif());
    iop::c_sif1_dma_transport sif1(
        memory, ee_hardware.dmac(),
        kernel.cpu(), kernel.hardware());
    iop::c_sif0_dma_transport sif0(
        memory, ee_hardware.dmac(),
        kernel.cpu(), kernel.hardware());

    const iop::s_kernel_run_result boot =
        kernel.run(k_boot_instructions, 4096u);
    uint32_t ee_smflag = 0u;
    uint32_t ee_subaddr = 0u;
    uint32_t iop_smflag = 0u;
    uint32_t iop_dma10_before = 0u;
    if (reset_pc != iop::c_cpu::k_rom0_reset_pc ||
        boot.m_stop != iop::e_kernel_stop::budget_exhausted ||
        boot.m_executed != k_boot_instructions ||
        !ee_hardware.sif().get_reg(4u, &ee_smflag) ||
        !ee_hardware.sif().get_reg(2u, &ee_subaddr) ||
        !kernel.hardware().read(
            iop::c_hw::k_sif_smflag, 4u, &iop_smflag) ||
        !kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u,
            4u, &iop_dma10_before) ||
        (ee_smflag & 0x00030000u) != 0x00030000u ||
        iop_smflag != ee_smflag ||
        ee_subaddr == 0u ||
        (ee_subaddr & 0x0fu) != 0u ||
        ee_subaddr >= iop::c_cpu::k_ram_size ||
        static_cast<uint64_t>(ee_subaddr) + k_payload_bytes >
            iop::c_cpu::k_ram_size ||
        (iop_dma10_before & 0x01000000u) == 0u ||
        sif1.faulted() || sif0.faulted())
    {
        std::fprintf(stderr,
            "ROM0 readiness: stop=%s executed=%llu "
            "EE_SMFLAG=%08x IOP_SMFLAG=%08x SUBADDR=%08x "
            "DMA10_CHCR=%08x\n",
            stop_name(boot.m_stop),
            static_cast<unsigned long long>(boot.m_executed),
            ee_smflag, iop_smflag, ee_subaddr, iop_dma10_before);
        return failed("firmware readiness/receive buffer not established",
                      kernel);
    }

    // Verify mailbox ownership with a genuine EE acknowledgement.
    // No IOP flag was synthesized by the host: the guest published
    // both SMFLAG and its SIF receive-buffer address before this point.
    if (!ee_hardware.sif().set_reg(4u, 0x00030000u) ||
        !kernel.hardware().read(
            iop::c_hw::k_sif_smflag, 4u, &iop_smflag) ||
        (iop_smflag & 0x00030000u) != 0u)
    {
        return failed("EE SMFLAG acknowledge not visible to the IOP",
                      kernel);
    }

    const uint32_t packet[4] =
    {
        0x31464953u, 0x76543210u, 0xabcdef12u, 0xc0dec0deu
    };
    uint32_t before[4]{};
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!kernel.cpu().read32(ee_subaddr + i * 4u, &before[i]))
        {
            return failed("firmware receive-buffer read fault", kernel);
        }
    }

    // EE source-chain END tag with two inline quadwords:
    // an IOP DMA10 destination-chain header and sixteen payload bytes.
    // This is the same guest-facing source-chain mode OpenPS2BIOS
    // uses in its EE SifSetDma routine (CHCR=0x105, QWC=0).
    memory.write32(k_ee_tag + 0u, 0x70000002u);
    memory.write32(k_ee_tag + 4u, 0u);
    memory.write32(k_ee_tag + 8u, 0u);
    memory.write32(k_ee_tag + 12u, 0u);
    memory.write32(k_ee_tag + 16u, 0x40000000u | ee_subaddr);
    memory.write32(k_ee_tag + 20u, k_payload_bytes);
    memory.write32(k_ee_tag + 24u, 0u);
    memory.write32(k_ee_tag + 28u, 0u);
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        memory.write32(k_ee_tag + 32u + i * 4u, packet[i]);
    }

    uint32_t ee_dma6_before = 0u;
    uint32_t ee_dma6_after = 0u;
    uint32_t iop_dma10_after = 0u;
    uint32_t dicr2 = 0u;
    uint32_t istat = 0u;
    if (!read_ee(ee_hardware.dmac(), 0u, &ee_dma6_before) ||
        !write_ee(ee_hardware.dmac(), 0x10u, 0u) ||
        !write_ee(ee_hardware.dmac(), 0x20u, 0u) ||
        !write_ee(ee_hardware.dmac(), 0x30u, k_ee_tag) ||
        !write_ee(ee_hardware.dmac(), 0u, 0x105u))
    {
        return failed("EE DMA6 source-chain register programming",
                      kernel);
    }

    uint32_t source_checksum = 0u;
    uint32_t destination_checksum = 0u;
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        uint32_t received = 0u;
        if (!kernel.cpu().read32(
                ee_subaddr + i * 4u, &received) ||
            received != packet[i] ||
            memory.read32(k_ee_tag + 32u + i * 4u) != packet[i])
        {
            std::fprintf(stderr,
                "SIF1 byte mismatch at guest %08x: got=%08x expected=%08x "
                "before=%08x\n",
                ee_subaddr + i * 4u, received, packet[i], before[i]);
            return failed("real firmware receiver data mismatch", kernel);
        }
        source_checksum += byte_sum(packet[i]);
        destination_checksum += byte_sum(received);
    }

    const bool status_reads =
        read_ee(ee_hardware.dmac(), 0u, &ee_dma6_after) &&
        kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u,
            4u, &iop_dma10_after) &&
        kernel.hardware().read(
            iop::c_hw::k_dma_icr2, 4u, &dicr2) &&
        kernel.hardware().read(
            iop::c_hw::k_i_stat, 4u, &istat);
    if (!status_reads || sif1.faulted() ||
        sif1.queued_bytes() != 32u ||
        sif1.delivered_bytes() != k_payload_bytes ||
        sif1.fifo_words() != 0u ||
        (ee_dma6_before & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (ee_dma6_after & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (iop_dma10_after & 0x01000000u) != 0u ||
        (ee_hardware.dmac().status() & (1u << 6u)) == 0u ||
        (dicr2 & (1u << 27u)) == 0u ||
        (istat & (1u << iop::c_hw::k_dma_irq)) == 0u)
    {
        std::fprintf(stderr,
            "SIF1 DMA state: ee_before=%08x ee_after=%08x "
            "iop_before=%08x iop_after=%08x dstat=%08x "
            "dicr2=%08x istat=%08x queued=%llu delivered=%llu\n",
            ee_dma6_before, ee_dma6_after,
            iop_dma10_before, iop_dma10_after,
            ee_hardware.dmac().status(), dicr2, istat,
            static_cast<unsigned long long>(sif1.queued_bytes()),
            static_cast<unsigned long long>(sif1.delivered_bytes()));
        return failed("genuine firmware DMA completion inconsistent",
                      kernel);
    }

    // Continue the original transport probe before testing native
    // firmware SIFCMD processing. No host callback handles commands.
    const iop::s_kernel_run_result continued = kernel.run(4096u, 256u);
    if (continued.m_stop != iop::e_kernel_stop::budget_exhausted)
    {
        std::fprintf(stderr,
            "IOP continuation stop=%s pc=%08x opcode=%08x\n",
            stop_name(continued.m_stop), continued.m_guest_pc,
            continued.m_opcode);
        return failed("guest stopped while processing DMA receipt",
                      kernel);
    }

    // Test real firmware command processing, not an HLE dispatcher.
    // The v0.6 ob_sif_cmd_build packet has a 16-byte header:
    // [size, extra=0, command, option] followed by payload words.
    uint32_t status_magic = 0u;
    uint32_t received_before = 0u;
    if (!kernel.cpu().read32(
            k_firmware_sif_status, &status_magic) ||
        status_magic != k_firmware_status_magic ||
        !kernel.cpu().read32(
            k_firmware_status_received, &received_before))
    {
        return failed("genuine firmware SIF status block absent", kernel);
    }

    // INIT_CMD opt=0 publishes an EE command buffer to the guest IOP.
    const uint32_t init_address[5] =
    {
        20u, 0u, 0x80000002u, 0u, k_ee_sifcmd_reply
    };
    uint32_t received_after_address = 0u;
    if (!send_firmware_sifcmd(
            memory, ee_hardware.dmac(), sif1, kernel,
            ee_subaddr, init_address, 5u) ||
        !run_until_firmware_receives(
            kernel, received_before, &received_after_address))
    {
        return failed("IOP did not process INIT_CMD buffer address",
                      kernel);
    }
    uint32_t command_id = 0u;
    uint32_t command_result = 0xffffffffu;
    uint32_t command_buffer = 0u;
    if (!kernel.cpu().read32(
            k_firmware_sif_status + 8u, &command_id) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 12u, &command_result) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 16u, &command_buffer) ||
        command_id != 0x80000002u ||
        command_result != 0u ||
        command_buffer != k_ee_sifcmd_reply)
    {
        std::fprintf(stderr,
            "INIT_CMD address: command=%08x result=%08x buffer=%08x\n",
            command_id, command_result, command_buffer);
        return failed("guest firmware rejected valid INIT_CMD address",
                      kernel);
    }

    // Arm EE DMA5 before requesting RPC initialization. OpenPS2BIOS
    // itself will build SET_SREG(RPCINIT,1) and send it through IOP
    // DMA9/SIF0; no host packet is placed into the EE reply buffer.
    for (uint32_t offset = 0u; offset < 64u; offset += 4u)
    {
        memory.write32(k_ee_sifcmd_reply - 16u + offset, 0xcdcdcdcdu);
    }
    if (!arm_firmware_sif0_receiver(ee_hardware.dmac()))
    {
        return failed("could not arm genuine EE DMA5 receiver", kernel);
    }

    // INIT_CMD opt=1 requests the firmware's own RPC readiness reply.
    const uint32_t request_rpc[4] =
    {
        16u, 0u, 0x80000002u, 1u
    };
    uint32_t received_after_rpc = 0u;
    if (!send_firmware_sifcmd(
            memory, ee_hardware.dmac(), sif1, kernel,
            ee_subaddr, request_rpc, 4u) ||
        !run_until_firmware_receives(
            kernel, received_after_address, &received_after_rpc))
    {
        return failed("IOP did not process RPC INIT_CMD", kernel);
    }

    uint32_t rpc_requested = 0u;
    uint32_t rpc_announced = 0u;
    uint32_t received_bytes = 0u;
    uint32_t last_argument = 0u;
    uint32_t dma9_chcr = 0u;
    uint32_t ee5_chcr = 0u;
    ee::s_ee_u128 ee5_word{};
    if (!kernel.cpu().read32(
            k_firmware_sif_status + 8u, &command_id) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 12u, &command_result) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 148u, &rpc_requested) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 152u, &rpc_announced) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 156u, &received_bytes) ||
        !kernel.cpu().read32(
            k_firmware_sif_status + 160u, &last_argument) ||
        !kernel.hardware().read(
            iop::c_hw::k_sif0_dma_base + 8u, 4u, &dma9_chcr) ||
        !ee::c_ee_dmac::mmio_read(
            &ee_hardware.dmac(), ee::c_ee_dmac::k_sif0_base,
            4u, &ee5_word) ||
        command_id != 0x80000002u || command_result != 0u ||
        rpc_requested != 1u || rpc_announced != 1u ||
        received_bytes != 16u || last_argument != 0u ||
        received_after_rpc != received_after_address + 1u ||
        sif0.faulted() || sif0.delivered_bytes() != 32u ||
        (dma9_chcr & 0x01000000u) != 0u ||
        (ee5_word.m_lo & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (ee_hardware.dmac().status() & (1u << 5u)) == 0u)
    {
        std::fprintf(stderr,
            "SIFCMD reply: command=%08x result=%08x rpc_req=%u "
            "rpc_ack=%u received=%u bytes=%u last_arg=%08x "
            "sif0_delivered=%llu dma9=%08x ee5=%08llx DSTAT=%08x\n",
            command_id, command_result, rpc_requested,
            rpc_announced, received_after_rpc, received_bytes,
            last_argument,
            static_cast<unsigned long long>(sif0.delivered_bytes()),
            dma9_chcr,
            static_cast<unsigned long long>(ee5_word.m_lo),
            ee_hardware.dmac().status());
        return failed("genuine firmware SIF0 RPC readiness exchange",
                      kernel);
    }
    ee5_chcr = static_cast<uint32_t>(ee5_word.m_lo);

    // The reply is actually constructed by ob_sif_cmd_build in the
    // firmware: a 24-byte SET_SREG command padded to 2 quadwords.
    // Verify the entire 32-byte transfer and its guard regions.
    const uint32_t expected_reply[8] =
    {
        24u, 0u, 0x80000001u, 0u, 0u, 1u, 0u, 0u
    };
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        const uint32_t actual = memory.read32(
            k_ee_sifcmd_reply + index * 4u);
        if (actual != expected_reply[index])
        {
            std::fprintf(stderr,
                "SIFCMD reply mismatch at EE %08x actual=%08x expected=%08x\n",
                k_ee_sifcmd_reply + index * 4u,
                actual, expected_reply[index]);
            return failed("firmware SIF0 reply payload mismatch", kernel);
        }
    }
    for (uint32_t offset = 0u; offset < 16u; offset += 4u)
    {
        if (memory.read32(
                k_ee_sifcmd_reply - 16u + offset) != 0xcdcdcdcdu)
        {
            return failed("firmware reply corrupted EE lower guard",
                          kernel);
        }
    }
    // Upper guard begins after the full 32-byte rounded response.
    for (uint32_t offset = 0u; offset < 16u; offset += 4u)
    {
        if (memory.read32(
                k_ee_sifcmd_reply + 32u + offset) != 0xcdcdcdcdu)
        {
            return failed("firmware reply corrupted EE upper guard",
                          kernel);
        }
    }

    std::ofstream report(argv[2], std::ios::binary);
    if (!report)
    {
        std::fprintf(stderr, "Could not open integration JSON report\n");
        return 2;
    }
    report <<
        "{\n"
        "  \"format\": \"openps2bios-authentic-sif1-v2\",\n"
        "  \"rom0_bytes\": " << rom.size() << ",\n"
        "  \"reset_pc\": " << reset_pc << ",\n"
        "  \"boot_instructions\": " << boot.m_executed << ",\n"
        "  \"boot_stop\": \"" << stop_name(boot.m_stop) << "\",\n"
        "  \"smflag_published\": " << ee_smflag << ",\n"
        "  \"smflag_after_ack\": " << iop_smflag << ",\n"
        "  \"firmware_receive_buffer\": " << ee_subaddr << ",\n"
        "  \"direction\": \"SIF1\",\n"
        "  \"ee_channel\": 6,\n"
        "  \"iop_channel\": 10,\n"
        "  \"source\": " << (k_ee_tag + 32u) << ",\n"
        "  \"destination\": " << ee_subaddr << ",\n"
        "  \"requested_bytes\": " << k_payload_bytes << ",\n"
        "  \"transferred_bytes\": " << k_payload_bytes << ",\n"
        "  \"total_sif1_transferred_bytes\": " << sif1.delivered_bytes() << ",\n"
        "  \"source_checksum\": " << source_checksum << ",\n"
        "  \"destination_checksum\": " << destination_checksum << ",\n"
        "  \"ee_chcr_before\": " << ee_dma6_before << ",\n"
        "  \"ee_chcr_after\": " << ee_dma6_after << ",\n"
        "  \"iop_chcr_before\": " << iop_dma10_before << ",\n"
        "  \"iop_chcr_after\": " << iop_dma10_after << ",\n"
        "  \"ee_dstat\": " << ee_hardware.dmac().status() << ",\n"
        "  \"iop_dicr2\": " << dicr2 << ",\n"
        "  \"iop_istat\": " << istat << ",\n"
        "  \"continuation_stop\": \""
            << stop_name(continued.m_stop) << "\",\n"
        "  \"guest_pc\": " << kernel.cpu().state().m_pc << ",\n"
        "  \"sifcmd_first_received\": " << received_after_address << ",\n"
        "  \"sifcmd_second_received\": " << received_after_rpc << ",\n"
        "  \"sifcmd_last_command\": " << command_id << ",\n"
        "  \"sifcmd_result\": " << command_result << ",\n"
        "  \"sifcmd_ee_buffer\": " << command_buffer << ",\n"
        "  \"sifcmd_rpc_requested\": " << rpc_requested << ",\n"
        "  \"sifcmd_rpc_announced\": " << rpc_announced << ",\n"
        "  \"sifcmd_sif0_reply_bytes\": " << sif0.delivered_bytes() << ",\n"
        "  \"sifcmd_ee_dma5_chcr\": " << ee5_chcr << "\n"
        "}\n";
    if (!report.good())
    {
        std::fprintf(stderr, "Could not write integration JSON report\n");
        return 2;
    }
    std::printf(
        "OpenPS2BIOS ROM0 guest SIF1: PASS "
        "smflag=%08x subaddr=%08x payload=%u IOP_DICR2=%08x\n",
        ee_smflag, ee_subaddr, k_payload_bytes, dicr2);
    std::printf(
        "OpenPS2BIOS guest SIFCMD: PASS command=%08x "
        "reply_bytes=%llu ee_buffer=%08x rpc_announced=%u\n",
        command_id,
        static_cast<unsigned long long>(sif0.delivered_bytes()),
        command_buffer, rpc_announced);
    return 0;
}
