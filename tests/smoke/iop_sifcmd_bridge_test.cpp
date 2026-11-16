#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP guest SIFCMD: %s\n", message);
    return 1;
}

bool write_call(
    iop::c_cpu& cpu, uint32_t at, uint32_t target
)
{
    // jal target; nop; break. The R3000 must execute the actual
    // installed HLE import stub and its syscall before breaking.
    const uint32_t jal = 0x0c000000u |
        ((target >> 2u) & 0x03ffffffu);
    return cpu.write32(at, jal) &&
           cpu.write32(at + 4u, 0u) &&
           cpu.write32(at + 8u, 0x0000000du);
}

void descriptor(
    ee::ee_memory& memory,
    uint32_t address, uint32_t src, uint32_t dst, uint32_t size
)
{
    memory.write32(address, src);
    memory.write32(address + 4u, dst);
    memory.write32(address + 8u, size);
    memory.write32(address + 12u, 0u);
}

void command(
    ee::ee_memory& memory, uint32_t address,
    uint32_t cid, uint32_t first, uint32_t second
)
{
    memory.write32(address + 0u, 24u);
    memory.write32(address + 4u, 0u);
    memory.write32(address + 8u, cid);
    memory.write32(address + 12u, 0u);
    memory.write32(address + 16u, first);
    memory.write32(address + 20u, second);
    memory.write32(address + 24u, 0u);
    memory.write32(address + 28u, 0u);
}
} // namespace

int main()
{
    constexpr uint32_t k_list = 0x11000u;
    constexpr uint32_t k_source = 0x12000u;
    constexpr uint32_t k_data_source = 0x13000u;
    constexpr uint32_t k_destination = 0x14000u;
    constexpr uint32_t k_entry = 0x1c000u;

    ee::ee_memory ee_ram(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&ee_ram);
    dmac.reset();

    iop::c_kernel kernel;
    iop::c_sifcmd_bridge commands(kernel, sif);
    iop::c_sif1_bridge transport(
        ee_ram, kernel.cpu(), dmac, &commands
    );
    if (!commands.install() ||
        !write_call(
            kernel.cpu(), k_entry,
            iop::c_sifcmd_bridge::k_init_cmd_stub))
    {
        return fail("could not install guest SIFCMD import stub");
    }

    uint32_t register_value = 0xffffffffu;
    command(
        ee_ram, k_source, 0x80000001u, 3u, 0x12345678u
    );
    descriptor(
        ee_ram, k_list, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 24u
    );

    // Merely installing imports is not equivalent to executing IOP
    // initialization. Never make SUBADDR/SMFLAG ready on install.
    if (!sif.get_reg(2u, &register_value) ||
        register_value != 0u ||
        commands.initialized() ||
        transport.set_dma(k_list, 1u, &register_value) ||
        dmac.status() != 0u)
    {
        return fail("unexecuted guest sifcmd accepted a command");
    }

    const uint32_t thread_id = kernel.create_thread(
        k_entry, 0u, 1u, 0x400u
    );
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return fail("could not start IOP guest initializer");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return commands.handle_syscall(active, service);
        }
    );

    const iop::s_kernel_run_result init_result =
        kernel.run(64u, 64u);
    uint32_t smflag = 0xffffffffu;
    if (init_result.m_stop != iop::e_kernel_stop::breakpoint ||
        init_result.m_guest_pc != k_entry + 8u ||
        !commands.initialized() ||
        !sif.get_reg(2u, &register_value) ||
        register_value != iop::c_sifcmd_bridge::k_command_buffer ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("R3000 sifcmd init did not publish only SUBADDR");
    }

    uint32_t dma_id = 0u;
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        dma_id != 1u ||
        commands.sreg(3u) != 0x12345678u ||
        (dmac.status() & (1u << 6u)) == 0u)
    {
        return fail("SIF1 SREG system command not delivered");
    }
    uint32_t iop_word = 0u;
    if (!kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 8u,
            &iop_word) ||
        iop_word != 0x80000001u)
    {
        return fail("IOP did not receive original SIFCMD packet");
    }

    dmac.acknowledge(6u);
    command(
        ee_ram, k_source, 0x80000002u, 0x00180000u, 0u
    );
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        dma_id != 2u ||
        commands.ee_receive_address() != 0x00180000u ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("EE command receive address or SMFLAG ownership");
    }

    // An unsupported RPC CID may not be mistaken for a completed
    // operation or reply. Check that earlier transfers in the same
    // descriptor list are not partially committed on rejection.
    dmac.acknowledge(6u);
    kernel.cpu().write32(k_destination, 0xdeadc0deu);
    ee_ram.write32(k_data_source, 0xabcdef98u);
    descriptor(
        ee_ram, k_list, k_data_source, k_destination, 16u
    );
    descriptor(
        ee_ram, k_list + 16u, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 24u
    );
    command(
        ee_ram, k_source, 0x80000009u, 0x80000006u, 0u
    );
    if (transport.set_dma(k_list, 2u, &dma_id) ||
        !kernel.cpu().read32(k_destination, &iop_word) ||
        iop_word != 0xdeadc0deu ||
        commands.ee_receive_address() != 0x00180000u ||
        dmac.status() != 0u)
    {
        return fail("unsupported RPC command caused a partial DMA");
    }
    if (!kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 8u,
            &iop_word) ||
        iop_word != 0x80000002u)
    {
        return fail("unsupported command overwrote prior packet");
    }

    // Even an ordinary transfer may not overlap the reserved command
    // buffer without passing the command validation protocol.
    descriptor(
        ee_ram, k_list, k_data_source,
        iop::c_sifcmd_bridge::k_command_buffer - 16u, 32u
    );
    if (transport.set_dma(k_list, 1u, &dma_id))
    {
        return fail("unaligned command-region crossing accepted");
    }

    // Let another guest R3000 thread call the imported SetSreg service.
    constexpr uint32_t k_set_entry = k_entry + 0x20u;
    if (!write_call(
            kernel.cpu(), k_set_entry,
            iop::c_sifcmd_bridge::k_set_sreg_stub))
    {
        return fail("could not author R3000 SetSreg call");
    }
    const uint32_t setter = kernel.create_thread(
        k_set_entry, 0u, 1u, 0x400u, 5u, 0xf00dbeefu
    );
    if (setter == 0u || !kernel.start_thread(setter))
    {
        return fail("could not start guest SREG writer");
    }
    const iop::s_kernel_run_result setter_result =
        kernel.run(64u, 64u);
    if (setter_result.m_stop != iop::e_kernel_stop::breakpoint ||
        commands.sreg(5u) != 0xf00dbeefu ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("guest SetSreg was not executed correctly");
    }

    std::printf("IOP guest SIFCMD: PASS\n");
    return 0;
}
