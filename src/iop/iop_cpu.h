#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iop
{
class c_hw;
// The PS2 IOP is an R3000A (MIPS I), separate from the EE R5900.
// This standalone, fail-closed interpreter is the execution foundation
// for the IOP kernel/IRX loader. It does not start an IOP or set SIF flags.
enum class e_step_result
{
    completed,
    syscall,
    breakpoint,
    unsupported,
    memory_fault,
    overflow,
    budget_exhausted
};

struct s_cpu_state
{
    uint32_t m_gpr[32]{};
    uint32_t m_hi = 0u;
    uint32_t m_lo = 0u;
    uint32_t m_pc = 0u;
    uint32_t m_next_pc = 4u;
    uint32_t m_cop0_status = 0u;
    uint32_t m_cop0_cause = 0u;
    uint32_t m_cop0_epc = 0u;
    uint32_t m_cop0_prid = 0u;
};

struct s_run_result
{
    e_step_result m_result = e_step_result::completed;
    uint32_t m_instructions = 0u;
    uint32_t m_pc = 0u;
    uint32_t m_opcode = 0u;
};

class c_cpu
{
public:
    static constexpr uint32_t k_ram_size = 2u * 1024u * 1024u;
    static constexpr uint32_t k_rom0_size = 4u * 1024u * 1024u;
    static constexpr uint32_t k_rom0_physical = 0x1fc00000u;
    static constexpr uint32_t k_rom0_reset_pc = 0xbfc00000u;

    c_cpu();
    void reset(uint32_t entry = 0u);
    void bind_hardware(c_hw* hardware);

    // The 4 MiB primary PS2 BIOS ROM is read-only at physical
    // 0x1fc00000, KSEG0 and KSEG1. No firmware is provided by the
    // emulator; the caller supplies a legal local ROM dump.
    bool install_rom0(const std::vector<uint8_t>& image);
    bool has_rom0() const;
    bool reset_rom0();

    const s_cpu_state& state() const;
    s_cpu_state& state();

    uint64_t cycles() const;
    bool read32(uint32_t address, uint32_t* value) const;
    bool write32(uint32_t address, uint32_t value);

    // Exactly one guest instruction, with architectural branch-delay slots.
    // Exceptions and unsupported instructions leave PC at the faulting word.
    e_step_result step(uint32_t* opcode = nullptr);
    s_run_result run(uint32_t instruction_budget);

    // After a syscall is explicitly serviced by a future IOP kernel, the
    // kernel may advance past its instruction. Never do so automatically.
    void resume_syscall();

private:
    bool read(uint32_t address, uint32_t width, uint32_t* value) const;
    bool write(uint32_t address, uint32_t width, uint32_t value);

    c_hw* m_hardware = nullptr;
    std::vector<uint8_t> m_ram;
    std::vector<uint8_t> m_rom0;
    s_cpu_state m_state{};
    uint64_t m_cycles = 0u;
};
} // namespace iop
