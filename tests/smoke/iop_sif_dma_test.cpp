#include "iop_sifman_bridge.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
void put16(std::vector<uint8_t>& bytes, uint32_t at, uint16_t value)
{
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        bytes[at + index] = static_cast<uint8_t>(
            value >> (index * 8u)
        );
    }
}

int fail(const char* message)
{
    std::fprintf(stderr, "IOP SIF DMA guest: %s\n", message);
    return 1;
}

std::vector<uint8_t> make_irx()
{
    // An original synthetic IRX whose R3000 instructions invoke
    // sifman ordinal 7 through an actual relocated guest import stub.
    std::vector<uint8_t> elf(0x200u, 0u);
    elf[0] = 0x7fu;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1u;
    elf[5] = 1u;
    elf[6] = 1u;
    put16(elf, 16u, 0xff80u);
    put16(elf, 18u, 8u);
    put32(elf, 28u, 0x34u);
    put16(elf, 40u, 52u);
    put16(elf, 42u, 32u);
    put16(elf, 44u, 2u);
    put32(elf, 0x34u, 1u);
    put32(elf, 0x38u, 0x100u);
    put32(elf, 0x44u, 0x60u);
    put32(elf, 0x48u, 0x80u);
    put32(elf, 0x54u, 0x70000080u);
    put32(elf, 0x58u, 0x180u);
    put32(elf, 0x184u, 0u);
    put32(elf, 0x188u, 0x50u);
    put32(elf, 0x18cu, 0x40u);
    put32(elf, 0x190u, 0x20u);
    put32(elf, 0x194u, 0x20u);
    put32(elf, 0x100u, 0x3c040001u); // lui a0,1
    put32(elf, 0x104u, 0x34844000u); // ori a0,a0,0x4000
    put32(elf, 0x108u, 0x24050002u); // addiu a1,zero,2
    put32(elf, 0x10cu, 0x0c00800du); // jal imported sifman #7
    put32(elf, 0x110u, 0u);         // delay nop
    put32(elf, 0x114u, 0x0000000du); // break

    put32(elf, 0x120u, 0x41e00000u);
    const char library[] = "sifman";
    for (uint32_t i = 0u; i < sizeof(library) - 1u; ++i)
    {
        elf[0x12cu + i] = static_cast<uint8_t>(library[i]);
    }
    put32(elf, 0x134u, 0x03e00008u);
    put32(elf, 0x138u, 0x24000007u);
    return elf;
}

bool set_sif0_chcr(ee::c_ee_dmac& dmac, uint32_t value)
{
    const ee::s_ee_u128 chcr{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_sif0_base, 4u, &chcr
    );
}

bool make_descriptor(
    iop::c_cpu& cpu, uint32_t at, uint32_t src,
    uint32_t dst, uint32_t size
)
{
    return cpu.write32(at, src) &&
           cpu.write32(at + 4u, dst) &&
           cpu.write32(at + 8u, size) &&
           cpu.write32(at + 12u, 0u);
}
} // namespace

int main()
{
    constexpr uint32_t k_list = 0x14000u;
    constexpr uint32_t k_source0 = 0x15000u;
    constexpr uint32_t k_source1 = 0x15100u;
    constexpr uint32_t k_destination0 = 0x00100000u;
    constexpr uint32_t k_destination1 = 0x00100100u;

    ee::ee_memory ee_ram(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&ee_ram);
    dmac.reset();

    iop::c_kernel kernel;
    iop::c_sifman_bridge bridge(
        kernel, sif, &dmac, &ee_ram
    );
    if (!bridge.install())
    {
        return fail("could not publish sifman guest import stubs");
    }
    const iop::s_module_load_result loaded =
        kernel.load_module(make_irx());
    if (loaded.m_status != iop::e_irx_load_result::success)
    {
        return fail("synthetic SIF DMA IRX was rejected");
    }

    if (!make_descriptor(
            kernel.cpu(), k_list,
            k_source0, k_destination0, 32u) ||
        !make_descriptor(
            kernel.cpu(), k_list + 16u,
            k_source1, k_destination1, 20u))
    {
        return fail("could not populate IOP DMA descriptor table");
    }

    for (uint32_t index = 0u; index < 8u; ++index)
    {
        if (!kernel.cpu().write32(
                k_source0 + index * 4u, 0xaabb0000u + index))
        {
            return fail("could not fill first IOP source");
        }
    }
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        if (!kernel.cpu().write32(
                k_source1 + index * 4u, 0xccdd0000u + index))
        {
            return fail("could not fill second IOP source");
        }
    }

    // The EE explicitly arms SIF0. No DMA is delivered or acknowledged
    // merely because an IOP import is linked or a thread is started.
    if (!set_sif0_chcr(dmac, 0x184u))
    {
        return fail("EE SIF0 receive chain could not be armed");
    }
    const uint32_t thread_id =
        kernel.create_module_thread(loaded.m_id, 1u);
    if (thread_id == 0u || !kernel.start_thread(thread_id) ||
        (dmac.status() & (1u << 5u)) != 0u ||
        ee_ram.read32(k_destination0) != 0u)
    {
        return fail("unexecuted IOP guest changed EE DMA state");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return bridge.handle_syscall(active, service);
        }
    );
    const iop::s_kernel_run_result run = kernel.run(64u, 64u);
    const iop::s_kernel_thread* after = kernel.thread(thread_id);
    uint32_t flag = 0xffffffffu;
    if (run.m_stop != iop::e_kernel_stop::breakpoint ||
        run.m_guest_pc != 0x00020014u ||
        after == nullptr || after->m_cpu.m_gpr[2] != 1u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        !sif.get_reg(4u, &flag) || flag != 0u)
    {
        return fail("guest sifman #7 did not deliver a real SIF0 completion");
    }

    for (uint32_t index = 0u; index < 8u; ++index)
    {
        if (ee_ram.read32(k_destination0 + index * 4u) !=
            0xaabb0000u + index)
        {
            return fail("first guest IOP DMA payload mismatch");
        }
    }
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        if (ee_ram.read32(k_destination1 + index * 4u) !=
            0xccdd0000u + index)
        {
            return fail("second guest IOP DMA payload mismatch");
        }
    }

    // Reject the entire descriptor list before writing *any* EE
    // payload when the second transfer is invalid.
    dmac.acknowledge(5u);
    ee_ram.write32(k_destination0, 0xdeadbeefu);
    kernel.cpu().state().m_gpr[4] = k_list;
    kernel.cpu().state().m_gpr[5] = 2u;
    if (!kernel.cpu().write32(k_list + 16u + 8u, 0xffffffffu) ||
        bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        ee_ram.read32(k_destination0) != 0xdeadbeefu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("invalid second DMA descriptor caused partial write");
    }
    if (!kernel.cpu().write32(k_list + 16u + 8u, 20u))
    {
        return fail("failed to restore valid descriptor");
    }

    // A transfer must not create EE DMA data/interrupts unless the
    // EE has actually enabled the SIF0 receive channel.
    if (!set_sif0_chcr(dmac, 0u) ||
        bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        ee_ram.read32(k_destination0) != 0xdeadbeefu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("IOP delivered DMA into an unarmed EE channel");
    }

    if (!set_sif0_chcr(dmac, 0x184u) ||
        !bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        kernel.cpu().state().m_gpr[2] != 2u ||
        ee_ram.read32(k_destination0) != 0xaabb0000u)
    {
        return fail("failed DMA incorrectly consumed an ID or data");
    }

    // The matched retail ARKD worker sends a single 96 KiB descriptor
    // from IOP 0x000dd2c0 to EE 0x01652400. The older 64 KiB cap
    // incorrectly rejected this valid, armed transfer.
    constexpr uint32_t k_large_source = 0x000dd2c0u;
    constexpr uint32_t k_large_destination = 0x01652400u;
    constexpr uint32_t k_large_size = 96u * 1024u;
    for (uint32_t index = 0u; index < k_large_size / 4u; ++index)
    {
        const uint32_t word = 0x5a430000u ^ (index * 0x010203u);
        if (!kernel.cpu().write32(k_large_source + index * 4u, word))
        {
            return fail("could not populate retail-size IOP DMA source");
        }
    }
    if (!make_descriptor(kernel.cpu(), k_list, k_large_source,
                         k_large_destination, k_large_size))
    {
        return fail("could not create retail-size DMA descriptor");
    }

    dmac.acknowledge(5u);
    ee_ram.write32(k_large_destination - 4u, 0xdeadbeefu);
    ee_ram.write32(k_large_destination, 0xabcdef01u);
    ee_ram.write32(k_large_destination + k_large_size, 0xfeedfaceu);
    kernel.cpu().state().m_gpr[4] = k_list;
    kernel.cpu().state().m_gpr[5] = 1u;
    if (!set_sif0_chcr(dmac, 0u) ||
        bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        ee_ram.read32(k_large_destination) != 0xabcdef01u ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("unarmed 96 KiB DMA changed EE state");
    }

    if (!set_sif0_chcr(dmac, 0x184u) ||
        !bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        kernel.cpu().state().m_gpr[2] != 3u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        ee_ram.read32(k_large_destination - 4u) != 0xdeadbeefu ||
        ee_ram.read32(k_large_destination + k_large_size) != 0xfeedfaceu)
    {
        return fail("armed 96 KiB DMA did not complete with intact guards");
    }
    for (uint32_t index = 0u; index < k_large_size / 4u; ++index)
    {
        const uint32_t expected = 0x5a430000u ^ (index * 0x010203u);
        if (ee_ram.read32(k_large_destination + index * 4u) != expected)
        {
            return fail("retail-size IOP DMA payload mismatch");
        }
    }

    // A later invalid descriptor must not commit the earlier valid
    // descriptor, acknowledge DMA5, or consume the next DMA ID.
    dmac.acknowledge(5u);
    ee_ram.write32(k_destination0, 0x1234abcdu);
    kernel.cpu().state().m_gpr[5] = 2u;
    if (!make_descriptor(kernel.cpu(), k_list,
                         k_source0, k_destination0, 32u) ||
        !make_descriptor(kernel.cpu(), k_list + 16u,
                         iop::c_cpu::k_ram_size - k_large_size + 4u,
                         k_large_destination, k_large_size) ||
        bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        ee_ram.read32(k_destination0) != 0x1234abcdu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("IOP-boundary 96 KiB descriptor caused partial DMA");
    }
    if (!make_descriptor(kernel.cpu(), k_list + 16u,
                         k_large_source,
                         32u * 1024u * 1024u - k_large_size + 4u,
                         k_large_size) ||
        bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        ee_ram.read32(k_destination0) != 0x1234abcdu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("EE-boundary 96 KiB descriptor caused partial DMA");
    }

    // Rejected operations leave the next genuine DMA ID available.
    kernel.cpu().state().m_gpr[5] = 1u;
    if (!make_descriptor(kernel.cpu(), k_list,
                         k_large_source, k_large_destination, k_large_size) ||
        !bridge.handle_syscall(
            kernel, iop::c_sifman_bridge::k_set_dma_syscall) ||
        kernel.cpu().state().m_gpr[2] != 4u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("rejected 96 KiB DMA consumed the following DMA ID");
    }

    // An IOP kernel without an EE DMA backend must not pretend to
    // accept an imported sifman SetDma operation.
    iop::c_kernel isolated;
    iop::c_sifman_bridge isolated_bridge(isolated, sif);
    if (!isolated_bridge.install() ||
        isolated_bridge.handle_syscall(
            isolated, iop::c_sifman_bridge::k_set_dma_syscall))
    {
        return fail("unconnected IOP sifman DMA returned fake success");
    }

    std::printf("IOP SIF DMA guest: PASS\n");
    return 0;
}
