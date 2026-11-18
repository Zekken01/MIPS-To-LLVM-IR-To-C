#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"
#include "iop_sif_rpc_bridge.h"
#include "iop_folder_sector_source.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "IOP LOADFILE RPC: %s\n", reason);
    return 1;
}

void write16(std::vector<uint8_t>& bytes, uint32_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void write32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

bool create_irx(const std::filesystem::path& location)
{
    std::vector<uint8_t> bytes(0x220u, 0u);
    bytes[0] = 0x7fu;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1u;
    bytes[5] = 1u;
    bytes[6] = 1u;
    write16(bytes, 16u, 0xff80u);
    write16(bytes, 18u, 8u);
    write32(bytes, 28u, 0x34u);
    write16(bytes, 40u, 52u);
    write16(bytes, 42u, 32u);
    write16(bytes, 44u, 2u);
    write32(bytes, 0x34u, 1u);
    write32(bytes, 0x38u, 0x100u);
    write32(bytes, 0x44u, 0x60u);
    write32(bytes, 0x48u, 0x80u);
    write32(bytes, 0x54u, 0x70000080u);
    write32(bytes, 0x58u, 0x180u);
    write32(bytes, 0x184u, 0u);
    write32(bytes, 0x188u, 0x50u);
    write32(bytes, 0x18cu, 0x40u);
    write32(bytes, 0x190u, 0x20u);
    write32(bytes, 0x194u, 0x20u);
    const char name[] = "RPCTEST";
    for (uint32_t i = 0u; i < sizeof(name); ++i)
    {
        bytes[0x19au + i] = static_cast<uint8_t>(name[i]);
    }
    write32(bytes, 0x100u, 0x00000000u); // initial guest nop
    write32(bytes, 0x104u, 0x0000000du); // guest breakpoint
    std::ofstream out(location, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    return out.good();
}

void descriptor(
    ee::ee_memory& memory,
    uint32_t at, uint32_t src, uint32_t dst, uint32_t size
)
{
    memory.write32(at + 0u, src);
    memory.write32(at + 4u, dst);
    memory.write32(at + 8u, size);
    memory.write32(at + 12u, 0u);
}

void packet(
    ee::ee_memory& memory, uint32_t at,
    const std::vector<uint32_t>& words
)
{
    for (uint32_t index = 0u; index < words.size(); ++index)
    {
        memory.write32(at + index * 4u, words[index]);
    }
}

bool init_sifcmd_guest(iop::c_kernel& kernel)
{
    constexpr uint32_t start = 0x0001c000u;
    const uint32_t jump = 0x0c000000u |
        (iop::c_sifcmd_bridge::k_init_cmd_stub >> 2u);
    if (!kernel.cpu().write32(start, jump) ||
        !kernel.cpu().write32(start + 4u, 0u) ||
        !kernel.cpu().write32(start + 8u, 0x0000000du))
    {
        return false;
    }
    const uint32_t thread_id = kernel.create_thread(
        start, 0u, 1u, 0x400u
    );
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return false;
    }
    const iop::s_kernel_run_result run = kernel.run(64u, 64u);
    return run.m_stop == iop::e_kernel_stop::breakpoint &&
           run.m_guest_pc == start + 8u;
}

struct s_temp_dir
{
    std::filesystem::path m_path;
    ~s_temp_dir()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
};

} // namespace

int main()
{
    constexpr uint32_t list = 0x10000u;
    constexpr uint32_t bind_source = 0x12000u;
    constexpr uint32_t call_source = 0x13000u;
    constexpr uint32_t args_source = 0x14000u;
    constexpr uint32_t ee_receive = 0x00180000u;
    constexpr uint32_t rpc_result = 0x00190000u;

    s_temp_dir folder{
        std::filesystem::temp_directory_path() /
        ("sif-rpc-" + std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch().count()))
    };
    std::filesystem::create_directories(folder.m_path);
    if (!create_irx(folder.m_path / "PADMAN.IRX"))
    {
        return fail("cannot write synthetic disc module");
    }
    {
        std::vector<uint8_t> image(0x100u, 0u);
        const std::string marker = "PsIIloadfile3040";
        std::copy(marker.begin(), marker.end(), image.begin() + 0x40u);
        std::ofstream out(folder.m_path / "IOPRP300.IMG", std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        if (!out.good())
        {
            return fail("cannot create reboot image metadata fixture");
        }
    }

    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&memory);
    dmac.reset();

    iop::c_kernel kernel;
    iop::c_sifcmd_bridge commands(kernel, sif);
    iop::c_disc_loader loader(kernel, folder.m_path);
    iop::c_folder_sector_source sectors(folder.m_path);
    if (!sectors.valid())
    {
        return fail("synthetic extracted disc could not mount");
    }
    iop::c_sif_rpc_bridge rpc(
        loader, kernel, commands, memory, dmac, &sectors
    );
    iop::c_sif1_bridge sif1(
        memory, kernel.cpu(), dmac, &commands, &rpc
    );

    if (!commands.install())
    {
        return fail("SIFCMD import registration failed");
    }
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t syscall)
        {
            return commands.handle_syscall(active, syscall);
        }
    );

    // An installed HLE bridge and a static on-disc IRX are not
    // scheduled guest work and must not advance the IOP clock.
    if (kernel.has_guest_work())
    {
        return fail("IOP scheduler reported work with no runnable module");
    }
    // The EE cannot dispatch any LOADFILE command merely because
    // a module exists on the host disc.
    const std::vector<uint32_t> bind = {
        36u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        1u, 0x11000u, 0x1234u, 0x15000u,
        iop::c_sif_rpc_bridge::k_loadfile_server,
        0u, 0u, 0u
    };
    packet(memory, bind_source, bind);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 36u
    );
    uint32_t dma_id = 0u;
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u)
    {
        return fail("RPC accepted before guest IOP init");
    }

    if (!init_sifcmd_guest(kernel) || !commands.initialized())
    {
        return fail("R3000 sceSifInitCmd not executed");
    }

    // Guest EE publishes an actual command receive buffer via SIF1.
    packet(memory, bind_source, {
        24u, 0u, 0x80000002u, 0u, ee_receive, 0u, 0u, 0u
    });
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 24u
    );
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        commands.ee_receive_address() != ee_receive)
    {
        return fail("SIFCMD EE receive-address handshake failed");
    }

    packet(memory, bind_source, bind);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 36u
    );
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 1u ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("RPC bind should queue until EE SIF0 is armed");
    }

    const ee::s_ee_u128 armed{0x184u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_sif0_base,
            4u, &armed) ||
        !rpc.pump() || rpc.pending_replies() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_loadfile_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        memory.read32(ee_receive + 40u) !=
            iop::c_sif_rpc_bridge::k_rpc_data_destination)
    {
        return fail("LOADFILE bind reply did not traverse armed SIF0");
    }

    // EE SDK sifrpc also uses a full 64-byte bind command, with
    // identity fields at identical offsets and a terminal 0x44 DMA.
    // Only a registered LOADFILE HLE service may answer this bind.
    const std::vector<uint32_t> bind64 = {
        64u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        5u, 0x11040u, 0x1234u, 0x15000u,
        iop::c_sif_rpc_bridge::k_loadfile_server,
        0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    packet(memory, bind_source, bind64);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 64u
    );
    memory.write32(list + 12u, 0x44u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        memory.read32(ee_receive + 0u) != 48u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 16u) != 5u ||
        memory.read32(ee_receive + 20u) != 0x11040u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_loadfile_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("64-byte LOADFILE bind did not reply through SIF0");
    }

    // A syntactically valid 64-byte bind is not proof that an
    // arbitrary RPC server exists. Verify transactional rejection
    // and preserve the last completed EE receive packet.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 32u, 0x80000004u);
    iop::s_sif1_dma_diagnostic bind_rejection{};
    if (sif1.set_dma(list, 1u, &dma_id, &bind_rejection) ||
        bind_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        bind_rejection.m_rpc_server != 0x80000004u ||
        bind_rejection.m_rpc_client != 0x15000u ||
        memory.read32(ee_receive + 16u) != 5u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unknown 64-byte RPC bind mutated guest state");
    }

    // The next retail SDBZ bind targets CDVDFSV CdInit (0x80000592).
    // It uses the ordinary 64-byte SDK packet and terminal 0x44 DMA.
    // Only the registered HLE service gets an actual SIF0 reply.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 28u, 0x005688a8u);
    memory.write32(bind_source + 32u,
                   iop::c_sif_rpc_bridge::k_cdvd_init_server);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        memory.read32(ee_receive + 0u) != 48u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 28u) != 0x005688a8u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_cdvd_init_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        memory.read32(ee_receive + 40u) !=
            iop::c_sif_rpc_bridge::k_rpc_data_destination ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        kernel.module(1u) != nullptr ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("64-byte CDVD Init BIND did not reply through SIF0");
    }

    // An unregistered CDVDFSV server remains unsupported. A valid
    // identity alone must not synthesize another server or interrupt.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 32u, 0x80000594u);
    iop::s_sif1_dma_diagnostic cdvd_rejection{};
    if (sif1.set_dma(list, 1u, &dma_id, &cdvd_rejection) ||
        cdvd_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_rejection.m_rpc_server != 0x80000594u ||
        cdvd_rejection.m_rpc_client != 0x005688a8u ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_cdvd_init_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVDFSV bind fabricated a response");
    }
    memory.write32(bind_source + 28u, 0x15000u);

    // These fields reproduce the 2026-11-18 12:14 retail SDBZ
    // SIF1 RPC header. Word[4] is record ID 5; word[8] is method 0.
    // The nonzero DEADBEF0 header destination is a valid IOP RPC
    // data address, not evidence of an unsupported packet.
    // Only result[3] changes, as in ps2-web-recomp's CDVDFSV HLE.
    const std::vector<uint32_t> cdvd_call = {
        0x440u, iop::c_sif_rpc_bridge::k_rpc_data_destination,
        iop::c_sif_rpc_bridge::k_call_cid, 0u,
        5u, 0x20561900u, 4u, 0x005688a8u,
        0u, 4u, rpc_result, 16u, 1u,
        iop::c_sif_rpc_bridge::k_cdvd_init_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, cdvd_call);
    packet(memory, args_source, {0u, 0u, 0u, 0u});
    packet(memory, rpc_result, {
        0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u
    });
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 4u);
    descriptor(memory, list + 16u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 28u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0x11111111u ||
        memory.read32(rpc_result + 4u) != 0x22222222u ||
        memory.read32(rpc_result + 8u) != 0x33333333u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        memory.read32(ee_receive + 16u) != 5u ||
        memory.read32(ee_receive + 20u) != 0x20561900u ||
        memory.read32(ee_receive + 24u) != 4u ||
        memory.read32(ee_receive + 28u) != 0x005688a8u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        kernel.module(1u) != nullptr ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("retail CDVD Init method 0 did not return through SIF0");
    }

    // A nonzero arbitrary header destination is not an accepted RPC
    // data destination, even though DEADBEF0 is valid.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 4u, 0xdeadbe00u);
    iop::s_sif1_dma_diagnostic invalid_rpc_destination{};
    if (sif1.set_dma(
            list, 2u, &dma_id, &invalid_rpc_destination) ||
        invalid_rpc_destination.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        invalid_rpc_destination.m_rpc_server !=
            iop::c_sif_rpc_bridge::k_cdvd_init_server ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("invalid RPC header destination committed DMA");
    }
    memory.write32(
        call_source + 4u,
        iop::c_sif_rpc_bridge::k_rpc_data_destination
    );

    // An unmodeled CdInit method must not write a success result,
    // emit a reply, or acknowledge a DMA that did not complete.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 32u, 6u);
    iop::s_sif1_dma_diagnostic cdvd_call_rejection{};
    if (sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_call_rejection.m_rpc_server !=
            iop::c_sif_rpc_bridge::k_cdvd_init_server ||
        cdvd_call_rejection.m_rpc_client != 0x005688a8u ||
        cdvd_call_rejection.m_rpc_packet_word_count != 16u ||
        cdvd_call_rejection.m_rpc_packet_words[0] != 0x440u ||
        cdvd_call_rejection.m_rpc_packet_words[8] != 6u ||
        cdvd_call_rejection.m_rpc_packet_words[13] !=
            (iop::c_sif_rpc_bridge::k_cdvd_init_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        cdvd_call_rejection.m_rpc_payload_source != args_source ||
        cdvd_call_rejection.m_rpc_payload_length != 4u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVD Init call fabricated completion");
    }
    // CDVDFSV CdInit must complete with a short receive buffer,
    // matching the reference HLE, without writing beyond its length.
    // The result[3] field is only written for a sixteen-byte reply.
    memory.write32(call_source + 32u, 0u);
    memory.write32(call_source + 44u, 12u);
    memory.write32(rpc_result + 12u, 0xa55a5aa5u);
    if (!sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::none ||
        memory.read32(rpc_result + 12u) != 0xa55a5aa5u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("CDVD Init short reply did not complete safely");
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);

    // The real SIF packet's advertised send size cannot exceed the
    // staged DMA payload. Preserve its complete header on failure.
    memory.write32(call_source + 44u, 16u);
    memory.write32(call_source + 36u, 20u);
    if (sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_call_rejection.m_rpc_packet_words[9] != 20u ||
        cdvd_call_rejection.m_rpc_packet_words[11] != 16u ||
        cdvd_call_rejection.m_rpc_payload_length != 4u ||
        memory.read32(rpc_result + 12u) != 0xa55a5aa5u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("CDVD Init payload length violation committed DMA");
    }
    memory.write32(call_source + 36u, 4u);

    // Captured 13:05 retail LOADFILE version query: one 64-byte
    // terminal SIF1 descriptor; no payload, four-byte receive result.
    const std::vector<uint32_t> version_call = {
        64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        5u, 0x20561900u, 8u, 0x00564d40u,
        0xffu, 0u, rpc_result, 4u, 1u,
        iop::c_sif_rpc_bridge::k_loadfile_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, version_call);
    descriptor(memory, list, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);
    memory.write32(rpc_result, 0xa5a5a5a5u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        memory.read32(rpc_result) != 0xa5a5a5a5u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("version RPC fabricated metadata before reboot");
    }
    if (!rpc.configure_reboot_image_version(
            "rom0:UDNL cdrom0:\\IOPRP300.IMG;1"))
    {
        return fail("cannot read actual IOPRP LOADFILE version");
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.last_observation().m_cid !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.last_observation().m_server_id !=
            iop::c_sif_rpc_bridge::k_loadfile_server ||
        rpc.last_observation().m_method != 0xffu ||
        rpc.last_observation().m_send_size != 0u ||
        rpc.last_observation().m_recv_size != 4u ||
        rpc.last_observation().m_return_word != 0x30343033u ||
        rpc.last_observation().m_module_started ||
        memory.read32(rpc_result) != 0x30343033u ||
        memory.read32(ee_receive + 16u) != 5u ||
        memory.read32(ee_receive + 28u) != 0x00564d40u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("LOADFILE version query did not complete via SIF0");
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 36u, 4u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("version RPC accepted unexpected send payload");
    }
    memory.write32(call_source + 36u, 0u);

    // Restore the single-command descriptor used by the next BIND.
    // The CdInit CALL above used a two-descriptor payload+command DMA.
    descriptor(memory, list, bind_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);

    // Bind the IOPHEAP server observed in the retail SDBZ bootstrap.
    // The EE must see the actual server identity in the SIF0 reply.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 32u,
                   iop::c_sif_rpc_bridge::k_iop_heap_server);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        memory.read32(ee_receive + 0u) != 48u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_iop_heap_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        memory.read32(ee_receive + 40u) !=
            iop::c_sif_rpc_bridge::k_rpc_data_destination ||
        rpc.load_requests() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("64-byte IOPHEAP BIND did not reply through SIF0");
    }

    // IOPHEAP alloc/free must use the same allocator as linked IOP
    // modules and guest thread stacks, and cannot free those resources.
    const std::vector<uint32_t> heap_call = {
        56u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        6u, 0x11060u, 0x1234u, 0x15000u,
        1u, 16u, rpc_result, 4u, 1u,
        iop::c_sif_rpc_bridge::k_iop_heap_server ^
            iop::c_sif_rpc_bridge::k_server_xor
    };
    packet(memory, call_source, heap_call);
    packet(memory, args_source, {0x100u, 0u, 0u, 0u});
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 16u);
    descriptor(memory, list + 16u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 56u);
    memory.write32(list + 28u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) < iop::c_kernel::k_user_base ||
        memory.read32(rpc_result) >= iop::c_cpu::k_ram_size ||
        (memory.read32(rpc_result) & 0x0fu) != 0u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        (dmac.status() & (1u << 5u)) == 0u ||
        rpc.load_requests() != 0u)
    {
        return fail("IOPHEAP method 1 did not allocate IOP RAM");
    }
    const uint32_t heap_address = memory.read32(rpc_result);

    // Method 2 can only free a previously issued IOPHEAP block.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 32u, 2u);
    memory.write32(args_source, heap_address);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid)
    {
        return fail("IOPHEAP method 2 could not free its own block");
    }

    // IOPHEAP method 3 transfers the mounted file into an owned
    // IOPHEAP allocation; no arbitrary IOP RAM write is permitted.
    const uint32_t heap_file_buffer = kernel.allocate_heap(0x400u);
    if (heap_file_buffer == 0u)
    {
        return fail("IOPHEAP file copy destination allocation failed");
    }
    const std::string heap_file_path = "cdrom0:\\PADMAN.IRX;1";
    memory.write32(call_source + 32u, 3u);
    memory.write32(call_source + 36u, 48u);
    memory.write32(args_source, heap_file_buffer);
    for (size_t index = 0u; index <= heap_file_path.size(); ++index)
    {
        memory.write8(args_source + 4u + static_cast<uint32_t>(index),
                      index == heap_file_path.size() ? 0u :
                          static_cast<uint8_t>(heap_file_path[index]));
    }
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 48u);
    memory.write32(list + 28u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    uint32_t heap_file_magic = 0u;
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0u ||
        !kernel.cpu().read32(heap_file_buffer, &heap_file_magic) ||
        heap_file_magic != 0x464c457fu ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("IOPHEAP file-to-RAM did not copy real mounted bytes");
    }

    // The same file must not escape its allocated destination.
    memory.write32(args_source, heap_file_buffer + 0x300u);
    memory.write32(rpc_result, 0xccccccccu);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    iop::s_sif1_dma_diagnostic heap_rejection{};
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        heap_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        memory.read32(rpc_result) != 0xccccccccu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("IOPHEAP out-of-allocation transfer mutated guest RAM");
    }

    const std::string absent_path = "cdrom0:\\NOFILE.IRX;1";
    memory.write32(args_source, heap_file_buffer);
    for (size_t index = 0u; index <= absent_path.size(); ++index)
    {
        memory.write8(args_source + 4u + static_cast<uint32_t>(index),
                      index == absent_path.size() ? 0u :
                          static_cast<uint8_t>(absent_path[index]));
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0xfffffffeu ||
        !kernel.cpu().read32(heap_file_buffer, &heap_file_magic) ||
        heap_file_magic != 0x464c457fu)
    {
        return fail("IOPHEAP missing file falsely reported success");
    }

    memory.write32(call_source + 36u, 8u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        heap_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        rpc.pending_replies() != 0u)
    {
        return fail("IOPHEAP truncated pathname was accepted");
    }
    memory.write32(call_source + 32u, 9u);
    memory.write32(call_source + 36u, 16u);
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 16u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        rpc.pending_replies() != 0u)
    {
        return fail("unknown IOPHEAP method was accepted");
    }
    memory.write32(call_source + 32u, 5u);

    // IOPHEAP query methods expose actual IOP memory capacity.
    memory.write32(call_source + 32u, 5u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != iop::c_cpu::k_ram_size)
    {
        return fail("IOPHEAP method 5 reported incorrect RAM size");
    }
    memory.write32(call_source + 32u, 6u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) == 0u ||
        memory.read32(rpc_result) > iop::c_cpu::k_ram_size)
    {
        return fail("IOPHEAP method 6 reported invalid free maximum");
    }

    const std::string guest_path = "cdrom0:\\PADMAN.IRX;1";
    memory.write32(args_source, 0u);
    memory.write32(args_source + 4u, 0u);
    for (size_t i = 0u; i <= guest_path.size(); ++i)
    {
        memory.write8(
            args_source + 8u + static_cast<uint32_t>(i),
            i == guest_path.size() ? 0u :
                static_cast<uint8_t>(guest_path[i])
        );
    }

    const std::vector<uint32_t> call = {
        56u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        2u, 0x11020u, 0x1234u, 0x15000u,
        0u, 260u, rpc_result, 8u, 1u,
        iop::c_sif_rpc_bridge::k_loadfile_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, call);
    descriptor(
        memory, list, call_source,
        iop::c_sifcmd_bridge::k_command_buffer, 56u
    );
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.load_requests() != 0u)
    {
        return fail("LOADFILE call without parameter DMA accepted");
    }

    dmac.acknowledge(5u);
    descriptor(
        memory, list, args_source,
        iop::c_sif_rpc_bridge::k_rpc_data_destination, 260u
    );
    descriptor(
        memory, list + 16u, call_source,
        iop::c_sifcmd_bridge::k_command_buffer, 56u
    );
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        rpc.last_observation().m_method != 0u ||
        rpc.last_observation().m_server_id !=
            iop::c_sif_rpc_bridge::k_loadfile_server ||
        rpc.last_observation().m_guest_path != guest_path ||
        rpc.last_observation().m_load_status !=
            iop::e_disc_load_result::success ||
        !rpc.last_observation().m_module_started ||
        !kernel.has_guest_work() ||
        rpc.last_observation().m_module_id != 1u ||
        rpc.last_observation().m_thread_id == 0u ||
        rpc.load_requests() != 1u ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result) != 1u ||
        memory.read32(rpc_result + 4u) != 0u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        !kernel.module(1u))
    {
        return fail("LOADFILE RPC did not load and schedule requested IRX");
    }

    // The linked guest entrypoint must actually run on the R3000.
    const iop::s_kernel_run_result first_slice = kernel.run(1u);
    if (first_slice.m_stop != iop::e_kernel_stop::budget_exhausted ||
        first_slice.m_executed != 1u ||
        !kernel.has_guest_work() ||
        kernel.module(1u) == nullptr)
    {
        return fail("LOADFILE guest thread did not consume its first slice");
    }
    const iop::s_kernel_run_result executed = kernel.run(64u);
    if (executed.m_stop != iop::e_kernel_stop::breakpoint ||
        kernel.has_guest_work() ||
        executed.m_guest_pc != kernel.module(1u)->m_irx.m_entry + 4u)
    {
        return fail("LOADFILE module entrypoint never ran");
    }

    // The modeled server responds with an error for a request that
    // cannot be found, without manufacturing another module ID.
    const std::string missing = "cdrom0:\\MISSING.IRX;1";
    for (size_t i = 0u; i <= missing.size(); ++i)
    {
        memory.write8(
            args_source + 8u + static_cast<uint32_t>(i),
            i == missing.size() ? 0u :
                static_cast<uint8_t>(missing[i])
        );
    }
    dmac.acknowledge(5u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        rpc.last_observation().m_load_status !=
            iop::e_disc_load_result::missing_file ||
        rpc.last_observation().m_guest_path != missing ||
        rpc.last_observation().m_module_started ||
        rpc.last_observation().m_return_word != 0xffffffffu ||
        rpc.load_requests() != 2u ||
        memory.read32(rpc_result) != 0xffffffffu ||
        memory.read32(rpc_result + 4u) != 0xffffffffu ||
        kernel.module(2u) != nullptr)
    {
        return fail("failed LOADFILE request invented a successful module");
    }

    // Invalid/unknown server IDs fail before any descriptor in the
    // list modifies guest RAM or the IOP module registry.
    memory.write32(call_source + 13u * 4u, 0x12345678u);
    dmac.acknowledge(6u);
    const uint32_t old_word = memory.read32(
        rpc_result
    );
    if (sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != old_word ||
        rpc.load_requests() != 2u ||
        (dmac.status() & (1u << 6u)) != 0u)
    {
        return fail("unknown RPC server caused partial effects");
    }

    uint32_t smflag = 0xffffffffu;
    if (!sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("LOADFILE RPC fabricated IOP SMFLAG readiness");
    }

    // The guest's second initialization command has a 16-byte
    // header (CID INIT_CMD, opt=1), not a 20-byte address payload.
    // The IOP must return SET_SREG[0]=1 through the already-armed
    // EE SIF0 receiver, not directly mutate an EE register.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    packet(memory, bind_source, {
        16u, 0u, 0x80000002u, 1u
    });
    memory.write32(ee_receive + 20u, 0xdeadbeefu);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 16u
    );
    memory.write32(list + 12u, 0x44u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        memory.read32(ee_receive + 0u) != 24u ||
        memory.read32(ee_receive + 8u) != 0x80000001u ||
        memory.read32(ee_receive + 12u) != 0u ||
        memory.read32(ee_receive + 16u) != 0u ||
        memory.read32(ee_receive + 20u) != 1u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        commands.ee_receive_address() != ee_receive ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("INIT_CMD option 1 did not deliver SREG via SIF0");
    }

    // No other 16-byte option value is a valid initialization
    // request, and it must not raise either DMA completion bit.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 12u, 2u);
    iop::s_sif1_dma_diagnostic reject{};
    if (sif1.set_dma(list, 1u, &dma_id, &reject) ||
        reject.m_failure !=
            iop::e_sif1_dma_failure::unsupported_sifcmd_packet ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported INIT_CMD option generated a reply");
    }


    // Exercise the entire implemented CDVDFSV service family through
    // actual SIF1 descriptor lists, SIF0 replies and mounted disc bytes.
    // These tests are independent of the retail SDBZ boot sequence.
    const uint32_t cdvd_servers[] = {
        iop::c_sif_rpc_bridge::k_cdvd_s_server,
        iop::c_sif_rpc_bridge::k_cdvd_n_server,
        iop::c_sif_rpc_bridge::k_cdvd_stream_server,
        iop::c_sif_rpc_bridge::k_cdvd_search_server,
        iop::c_sif_rpc_bridge::k_cdvd_ready_server,
        iop::c_sif_rpc_bridge::k_cdvd_ready_alt_server
    };
    for (uint32_t server : cdvd_servers)
    {
        std::vector<uint32_t> service_bind = bind64;
        service_bind[8] = server;
        packet(memory, bind_source, service_bind);
        descriptor(memory, list, bind_source,
                   iop::c_sifcmd_bridge::k_command_buffer, 64u);
        memory.write32(list + 12u, 0x44u);
        dmac.acknowledge(5u);
        dmac.acknowledge(6u);
        if (!sif1.set_dma(list, 1u, &dma_id) ||
            memory.read32(ee_receive + 32u) !=
                iop::c_sif_rpc_bridge::k_bind_cid ||
            memory.read32(ee_receive + 36u) !=
                (server ^ iop::c_sif_rpc_bridge::k_server_xor) ||
            (dmac.status() & (1u << 5u)) == 0u)
        {
            return fail("CDVDFSV server bind not delivered via SIF0");
        }
    }

    const auto cdvd_rpc_call = [&](uint32_t server, uint32_t method,
                               uint32_t send_size, uint32_t recv_size,
                               iop::s_sif1_dma_diagnostic* diagnostic = nullptr) -> bool
    {
        packet(memory, call_source, {
            64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
            8u, 0x110a0u, 0x1234u, 0x005688a8u,
            method, send_size, rpc_result, recv_size, 1u,
            server ^ iop::c_sif_rpc_bridge::k_server_xor, 0u, 0u
        });
        descriptor(memory, list, args_source,
                   iop::c_sif_rpc_bridge::k_rpc_data_destination,
                   send_size);
        descriptor(memory, list + 16u, call_source,
                   iop::c_sifcmd_bridge::k_command_buffer, 64u);
        memory.write32(list + 28u, 0x44u);
        dmac.acknowledge(5u);
        dmac.acknowledge(6u);
        return sif1.set_dma(list, 2u, &dma_id, diagnostic);
    };

    packet(memory, args_source, {0u, 0u, 0u, 0u});
    packet(memory, rpc_result, {0x11111111u, 0x22222222u, 0u, 0u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x03u, 16u, 4u) ||
        memory.read32(rpc_result) != 0x14u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x05u, 16u, 8u) ||
        memory.read32(rpc_result) != 1u ||
        memory.read32(rpc_result + 4u) != 0u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x27u, 16u, 12u) ||
        memory.read32(rpc_result + 8u) != 0u)
    {
        return fail("CDVDFSV S command results were incorrect");
    }
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_ready_server,
                   0u, 16u, 4u) ||
        memory.read32(rpc_result) != 2u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_ready_alt_server,
                   0u, 16u, 4u) ||
        memory.read32(rpc_result) != 2u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x04u, 16u, 4u) ||
        memory.read32(rpc_result) != 1u)
    {
        return fail("CDVDFSV media-backed status query failed");
    }

    iop::s_folder_file_info padman{};
    if (!sectors.find_file("cdrom0:\\PADMAN.IRX;1", &padman) ||
        padman.m_size != 0x220u)
    {
        return fail("CDVDFSV test disc file lookup failed");
    }
    packet(memory, args_source, {padman.m_lba, 1u, 0x001a0000u, 0u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x01u, 16u, 4u) ||
        memory.read32(rpc_result) != 0u ||
        memory.read32(0x001a0000u) != 0x464c457fu)
    {
        return fail("CDVDFSV N sector read did not copy real disc data");
    }
    packet(memory, args_source, {padman.m_lba, 1u, 0x00040000u, 0u});
    uint32_t iop_readback = 0u;
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x0du, 16u, 4u) ||
        !kernel.cpu().read32(0x00040000u, &iop_readback) ||
        iop_readback != 0x464c457fu)
    {
        return fail("CDVDFSV IOP sector read did not write actual IOP RAM");
    }

    const std::string cdvd_path = "cdrom0:\\PADMAN.IRX;1";
    packet(memory, args_source, {0xaabbccddu, 0x11223344u});
    for (size_t index = 0u; index <= cdvd_path.size(); ++index)
    {
        memory.write8(
            args_source + 0x24u + static_cast<uint32_t>(index),
            index == cdvd_path.size() ? 0u :
                static_cast<uint8_t>(cdvd_path[index])
        );
    }
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_search_server,
                   0u, 0x80u, 4u) ||
        memory.read32(rpc_result) != 1u ||
        memory.read32(args_source) != padman.m_lba ||
        memory.read32(args_source + 4u) != padman.m_size)
    {
        return fail("CDVDFSV search result did not match mounted ISO map");
    }

    packet(memory, args_source, {padman.m_lba, 1u, 0x001a0800u, 1u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x09u, 16u, 4u) ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x0cu, 16u, 4u) ||
        memory.read32(rpc_result) != 0x12u)
    {
        return fail("CDVDFSV stream-start state did not become visible");
    }
    packet(memory, args_source, {0u, 1u, 0x001a0800u, 2u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x09u, 16u, 4u) ||
        memory.read32(0x001a0800u) != 0x464c457fu)
    {
        return fail("CDVDFSV stream read did not use mounted sectors");
    }
    packet(memory, args_source, {0u, 0u, 0u, 3u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x09u, 16u, 4u) ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x0cu, 16u, 4u) ||
        memory.read32(rpc_result) != 0x0au)
    {
        return fail("CDVDFSV stream stop did not restore idle status");
    }

    // Reject unknown methods without memory or DMA side effects.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    packet(memory, args_source, {0u, 0u, 0u, 0u});
    const uint32_t preceding = memory.read32(rpc_result);
    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                  0xffffu, 16u, 4u) ||
        memory.read32(rpc_result) != preceding ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unknown CDVDFSV RPC invented a completion");
    }

    // Rejection categories are derived only from a genuine staged guest
    // packet. A diagnosis must never acknowledge a SIF0/6 DMA event,
    // alter the EE receive buffer or install an unknown service.
    iop::s_sif1_dma_diagnostic rejection{};
    const uint32_t unchanged_rpc_result = memory.read32(rpc_result);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (cdvd_rpc_call(0x8000beefu, 0u, 16u, 4u, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unknown_server ||
        rejection.m_rpc_server != 0x8000beefu ||
        rejection.m_rpc_packet_word_count != 16u ||
        memory.read32(rpc_result) != unchanged_rpc_result ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unknown SIFRPC server was not diagnosed fail-closed");
    }

    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                      0xffffu, 16u, 4u, &rejection) ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unsupported_method ||
        memory.read32(rpc_result) != unchanged_rpc_result ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVDFSV method was not diagnosed");
    }

    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                      0x01u, 4u, 4u, &rejection) ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::invalid_arguments ||
        rpc.pending_replies() != 0u)
    {
        return fail("truncated CDVDFSV arguments were not diagnosed");
    }

    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_stream_server,
                      1u, 16u, 4u, &rejection) ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unavailable_backend ||
        rpc.pending_replies() != 0u)
    {
        return fail("unscheduled CDVD stream RPC was not diagnosed");
    }

    // A valid first descriptor cannot partially write IOP RAM if a
    // second descriptor carries an unsupported RPC packet.
    const uint32_t guarded_iop_address = 0x00068000u;
    uint32_t guarded_before = 0u;
    if (!kernel.cpu().read32(guarded_iop_address, &guarded_before))
    {
        return fail("unable to inspect IOP rollback sentinel");
    }
    packet(memory, args_source, {0xdec0de11u, 0x12345678u, 0u, 0u});
    descriptor(memory, list, args_source, guarded_iop_address, 16u);
    // The rejected unknown-server call packet is still in call_source
    // only after explicitly restoring it (the previous calls replaced it).
    packet(memory, call_source, {
        64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        8u, 0x110a0u, 0x1234u, 0x005688a8u,
        0u, 16u, rpc_result, 4u, 1u,
        0x8000beefu ^ iop::c_sif_rpc_bridge::k_server_xor, 0u, 0u
    });
    descriptor(memory, list + 16u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 28u, 0x44u);
    uint32_t guarded_after = 0u;
    if (sif1.set_dma(list, 2u, &dma_id, &rejection) ||
        rejection.m_descriptor_index != 1u ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unknown_server ||
        !kernel.cpu().read32(guarded_iop_address, &guarded_after) ||
        guarded_after != guarded_before ||
        memory.read32(rpc_result) != unchanged_rpc_result ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("rejected second RPC partially committed prior IOP DMA");
    }

    // An explicitly successful prepare clears the diagnostic without
    // altering its caller's guest memory or delivering a reply.
    iop::c_sif_rpc_bridge::s_request staged{};
    iop::e_rpc_rejection status = iop::e_rpc_rejection::unknown_server;
    const std::vector<uint32_t> valid_bind = {
        36u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        1u, 0x11000u, 0x1234u, 0x15000u,
        iop::c_sif_rpc_bridge::k_loadfile_server
    };
    if (!rpc.prepare_command(valid_bind, 36u, 0u, 0u, &staged, &status) ||
        status != iop::e_rpc_rejection::none ||
        staged.m_server_id != iop::c_sif_rpc_bridge::k_loadfile_server ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result) != unchanged_rpc_result)
    {
        return fail("accepted RPC preview changed guest state or reason");
    }

    // Reset may not retain the previous IOPRP's LOADFILE version.
    rpc.reset_for_reboot();
    packet(memory, call_source, version_call);
    descriptor(memory, list, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u)
    {
        return fail("reset retained stale LOADFILE version");
    }
    std::printf("IOP LOADFILE RPC: PASS\n");
    return 0;
}
