#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"
#include "iop_sif_rpc_bridge.h"
#include "iop_folder_sector_source.h"

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
    write32(bytes, 0x100u, 0x0000000du); // guest breakpoint
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

    // Retail SDBZ's first CdInit request is method 5, carried by
    // a two-descriptor SIF1 RPC call with a full 64-byte SDK packet.
    // Only result[3] changes, as in ps2-web-recomp's CDVDFSV HLE.
    const std::vector<uint32_t> cdvd_call = {
        0x440u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        7u, 0x11080u, 0x1234u, 0x005688a8u,
        5u, 16u, rpc_result, 16u, 1u,
        iop::c_sif_rpc_bridge::k_cdvd_init_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, cdvd_call);
    packet(memory, args_source, {5u, 0u, 0u, 0u});
    packet(memory, rpc_result, {
        0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u
    });
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 16u);
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
        memory.read32(ee_receive + 28u) != 0x005688a8u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        kernel.module(1u) != nullptr ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("CDVD Init method 5 did not return through SIF0");
    }

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
        cdvd_call_rejection.m_rpc_payload_length != 16u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVD Init call fabricated completion");
    }
    // Method 5 with no room for its sixteen-byte reply is also refused.
    memory.write32(call_source + 32u, 5u);
    memory.write32(call_source + 44u, 12u);
    if (sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_call_rejection.m_rpc_packet_words[8] != 5u ||
        cdvd_call_rejection.m_rpc_packet_words[11] != 12u ||
        cdvd_call_rejection.m_rpc_payload_length != 16u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("CDVD Init short reply buffer was accepted");
    }

    // The real SIF packet's advertised send size cannot exceed the
    // staged DMA payload. Preserve its complete header on failure.
    memory.write32(call_source + 44u, 16u);
    memory.write32(call_source + 36u, 20u);
    if (sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_call_rejection.m_rpc_packet_words[9] != 20u ||
        cdvd_call_rejection.m_rpc_packet_words[11] != 16u ||
        cdvd_call_rejection.m_rpc_payload_length != 16u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("CDVD Init payload length violation committed DMA");
    }
    memory.write32(call_source + 36u, 16u);

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

    // IOPHEAP method 3 (file-to-RAM) is deliberately not accepted:
    // it needs a separately verified IOP file transport.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 32u, 3u);
    iop::s_sif1_dma_diagnostic heap_rejection{};
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        heap_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported IOPHEAP file load fabricated success");
    }

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
    const iop::s_kernel_run_result executed = kernel.run(64u);
    if (executed.m_stop != iop::e_kernel_stop::breakpoint ||
        kernel.module(1u) == nullptr ||
        executed.m_guest_pc != kernel.module(1u)->m_irx.m_entry)
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
                               uint32_t send_size, uint32_t recv_size) -> bool
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
        return sif1.set_dma(list, 2u, &dma_id);
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

    std::printf("IOP LOADFILE RPC: PASS\n");
    return 0;
}
