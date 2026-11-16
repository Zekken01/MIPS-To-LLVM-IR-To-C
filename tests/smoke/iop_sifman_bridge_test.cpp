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
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[at + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

int fail(const char* reason)
{
    std::fprintf(stderr, "IOP SIFMAN bridge: %s\n", reason);
    return 1;
}

std::vector<uint8_t> make_irx()
{
    // Synthetic, non-copyrighted IOP module. It calls sifman #24 with
    // 0x60000 and then sifman #23 through real IRX import-table stubs.
    std::vector<uint8_t> image(0x200u, 0u);
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 1u;
    image[5] = 1u;
    image[6] = 1u;
    put16(image, 16u, 0xff80u);
    put16(image, 18u, 8u);
    put32(image, 28u, 0x34u);
    put16(image, 40u, 52u);
    put16(image, 42u, 32u);
    put16(image, 44u, 2u);

    put32(image, 0x34u, 1u);           // PT_LOAD
    put32(image, 0x38u, 0x100u);
    put32(image, 0x44u, 0x70u);
    put32(image, 0x48u, 0x80u);

    put32(image, 0x54u, 0x70000080u);  // PT_SCE_IOPMOD
    put32(image, 0x58u, 0x180u);
    put32(image, 0x184u, 0u);          // entry
    put32(image, 0x188u, 0x60u);       // gp
    put32(image, 0x18cu, 0x40u);
    put32(image, 0x190u, 0x20u);
    put32(image, 0x194u, 0x20u);

    put32(image, 0x100u, 0x3c040006u); // lui a0,6
    put32(image, 0x104u, 0x0c00800fu); // jal 0x2003c (#24 import)
    put32(image, 0x108u, 0x00000000u); // delay nop
    put32(image, 0x10cu, 0x0c00800du); // jal 0x20034 (#23 import)
    put32(image, 0x110u, 0x00000000u); // delay nop
    put32(image, 0x114u, 0x08008005u); // j 0x20014 (idle loop)
    put32(image, 0x118u, 0x00000000u); // delay nop

    put32(image, 0x120u, 0x41e00000u); // IOP import marker
    const char name[] = "sifman";
    for (uint32_t i = 0u; i < sizeof(name) - 1u; ++i)
    {
        image[0x12cu + i] = static_cast<uint8_t>(name[i]);
    }
    put32(image, 0x134u, 0x03e00008u); // jr ra
    put32(image, 0x138u, 0x24000017u); // addiu zero,zero,23
    put32(image, 0x13cu, 0x03e00008u); // jr ra
    put32(image, 0x140u, 0x24000018u); // addiu zero,zero,24
    return image;
}
} // namespace

int main()
{
    iop::c_kernel kernel;
    ee::c_ee_sif ee_sif;
    ee_sif.reset();

    // Without the sifman exports, a real IRX loader must reject this
    // import table and leave the guest-visible SMFLAG at reset state.
    const std::vector<uint8_t> image = make_irx();
    const iop::s_module_load_result missing =
        kernel.load_module(image);
    uint32_t value = 0xffffffffu;
    if (missing.m_status !=
            iop::e_irx_load_result::unresolved_import ||
        !ee_sif.get_reg(4u, &value) || value != 0u)
    {
        return fail("unknown import fabricated SIF readiness");
    }

    iop::c_sifman_bridge bridge(kernel, ee_sif);
    if (!bridge.install())
    {
        return fail("failed to install guest-callable sifman HLE stubs");
    }

    // Registering HLE exports must not itself publish any IOP flag.
    if (!ee_sif.get_reg(4u, &value) || value != 0u)
    {
        return fail("install() fabricated a SIF ready bit");
    }

    const iop::s_module_load_result module =
        kernel.load_module(image);
    if (module.m_status != iop::e_irx_load_result::success ||
        module.m_id != 1u)
    {
        return fail("could not link synthetic sifman imports");
    }

    const uint32_t id =
        kernel.create_module_thread(module.m_id, 1u);
    if (id == 0u || !kernel.start_thread(id))
    {
        return fail("could not schedule synthetic IOP module");
    }

    // Even a scheduled IOP module cannot change EE state until it is
    // actually executed. The syscall handler deliberately recognizes
    // only the two documented sifman ordinals.
    if (!ee_sif.get_reg(4u, &value) || value != 0u ||
        bridge.handle_syscall(kernel, 0xffffffffu))
    {
        return fail("unexecuted module or unknown syscall changed SIF");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return bridge.handle_syscall(active, service);
        }
    );
    const iop::s_kernel_run_result run =
        kernel.run(64u, 64u);
    if (run.m_stop != iop::e_kernel_stop::budget_exhausted ||
        run.m_executed != 64u ||
        !ee_sif.get_reg(4u, &value) || value != 0x60000u)
    {
        return fail("guest sifman #24 failed to publish SMFLAG");
    }

    // The second guest import is sceSifGetSMFlag. Its returned V0 must
    // match the value published through the same shared EE mailbox.
    const iop::s_kernel_thread* thread =
        kernel.thread(id);
    if (thread == nullptr ||
        thread->m_cpu.m_gpr[2] != 0x60000u)
    {
        return fail("IOP sifman #23 returned incorrect SMFLAG");
    }

    // EE can acknowledge (clear) IOP bits but cannot set or replace
    // IOP-owned registers. A later IOP set is OR-ed, not replaced.
    if (!ee_sif.set_reg(4u, 0x20000u) ||
        !ee_sif.get_reg(4u, &value) ||
        value != 0x40000u ||
        ee_sif.set_reg(2u, 0xffffffffu))
    {
        return fail("EE acknowledgements violated SIF register ownership");
    }

    if (!bridge.handle_syscall(kernel,
                              iop::c_sifman_bridge::k_set_smflag_syscall) ||
        !ee_sif.get_reg(4u, &value) ||
        value != 0x60000u)
    {
        return fail("subsequent IOP set did not OR with remaining flags");
    }

    std::printf("IOP SIFMAN guest bridge: PASS\n");
    return 0;
}
