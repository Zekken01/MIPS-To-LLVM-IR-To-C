#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"
#include "iop_sif_rpc_bridge.h"

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
    iop::c_sif_rpc_bridge rpc(loader, commands, memory, dmac);
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
        executed.m_guest_pc != 0x20000u)
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

    std::printf("IOP LOADFILE RPC: PASS\n");
    return 0;
}
