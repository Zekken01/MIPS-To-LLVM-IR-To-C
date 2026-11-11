#include "ee_dispatcher.h"

#include <cstdio>
#include <initializer_list>
#include <string>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name)
{
    if (ok)
    {
        ++g_pass;
        std::printf("PASS: %s\n", name);
    }
    else
    {
        ++g_fail;
        std::printf("FAIL: %s\n", name);
    }
}

uint32_t caller_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    if (cpu->m_pc == 0x00100000u)
    {
        cpu->m_gpr[31].m_lo = 0x00100008u;
        cpu->m_pc = 0x00102000u;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    if (cpu->m_pc == 0x00100008u)
    {
        cpu->m_gpr[2].m_lo = 0x55u;
        cpu->m_pc = 0xDEAD0000u;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
}

uint32_t callee_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    cpu->m_pc = static_cast<uint32_t>(cpu->m_gpr[31].m_lo);
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

uint32_t jalr_caller_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    if (cpu->m_pc == 0x00200000u)
    {
        // Model the JALR's target capture, encoded rd link, and delay slot:
        // the captured target must survive rs being overwritten by the slot.
        const uint64_t captured_target = cpu->m_gpr[8].m_lo;
        cpu->m_gpr[31].m_lo = 0x00200008u;
        cpu->m_gpr[8].m_lo = cpu->m_gpr[31].m_lo;
        cpu->m_pc = static_cast<uint32_t>(captured_target);
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    if (cpu->m_pc == 0x00200008u)
    {
        cpu->m_gpr[17].m_lo = 0x66u;
        cpu->m_pc = 0xBADC0DEu;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
}

uint32_t external_return_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    cpu->m_pc = 0xF0000000u;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

uint32_t zero_initial_pc_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    // Generated entry switches treat PC zero as the function's initial entry.
    if (cpu->m_pc == 0 || cpu->m_pc == 0x00400000u)
    {
        cpu->m_gpr[3].m_lo = 0x33u;
        cpu->m_pc = 0xDEAD0000u;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
}

uint32_t dispatch_loop_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    // Deliberately return to this same translated block forever.
    cpu->m_pc = 0x00300000u;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
}

ee::s_recompiled_fn make_function(
    const char* name,
    uint32_t entry_pc,
    std::initializer_list<uint32_t> block_pcs)
{
    ee::s_recompiled_fn result{};
    result.name = name;
    result.vaddr = entry_pc;
    result.block_pcs.assign(block_pcs.begin(), block_pcs.end());
    result.block_count = static_cast<uint32_t>(result.block_pcs.size());
    return result;
}

bool is_exited(const ee::s_ee_dispatch_result& result, uint32_t reason)
{
    return result.status == ee::e_ee_dispatch_status::EXITED &&
           result.exit_reason == reason;
}

} // namespace

int main()
{
    using ee::e_ee_dispatch_status;
    using ee::e_ee_exit_reason;

    // A direct call dispatches to the callee. Its JR $ra returns to a caller
    // continuation block, which the same dispatcher invokes before returning
    // to the outer host.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("caller", 0x00100000u, {0x00100000u, 0x00100008u}));
        compiled.functions.push_back(
            make_function("callee", 0x00102000u, {0x00102000u}));
        const ee::t_ee_translated_entry entries[] =
        {
            caller_entry,
            callee_entry
        };
        const ee::s_ee_dispatch_table table{&compiled, entries, 2u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00100000u;
        cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 16u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 3u &&
              result.guest_pc == 0xDEAD0000u &&
              cpu.m_gpr[2].m_lo == 0x55u,
              "dispatch callee return into caller continuation");
    }

    // JALR's rs is overwritten by the delay slot, but the captured target
    // still selects the separately registered callee. The callee then returns
    // to the caller's second block.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("jalr_caller", 0x00200000u,
                          {0x00200000u, 0x00200008u}));
        compiled.functions.push_back(
            make_function("jalr_callee", 0x00202000u, {0x00202000u}));
        const ee::t_ee_translated_entry entries[] =
        {
            jalr_caller_entry,
            callee_entry
        };
        const ee::s_ee_dispatch_table table{&compiled, entries, 2u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00200000u;
        cpu.m_gpr[8].m_lo = 0x00202000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 16u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 3u &&
              result.guest_pc == 0xBADC0DEu &&
              cpu.m_gpr[8].m_lo == 0x00200008u &&
              cpu.m_gpr[17].m_lo == 0x66u,
              "dispatch dynamic JALR target and resume caller");
    }

    // A translated JR $ra can return outside the compiled set. The dispatcher
    // must propagate EE_EXIT_RETURN to the outer host instead of treating the
    // unmapped address as an error or invoking another function.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("external_return", 0x00280000u, {0x00280000u}));
        const ee::t_ee_translated_entry entries[] = {external_return_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00280000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN)) &&
              result.dispatch_count == 1u &&
              result.guest_pc == 0xF0000000u,
              "propagate return to external guest address");
    }

    // PC zero is a supported generated-function entry sentinel and must
    // resolve to the ELF entry's translated function, not appear unmapped.
    {
        ee::s_recompiled_elf compiled{};
        compiled.entry_point = 0x00400000u;
        compiled.functions.push_back(
            make_function("entry", 0x00400000u, {0x00400000u}));
        const ee::t_ee_translated_entry entries[] = {zero_initial_pc_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0u;
        cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 1u &&
              result.guest_pc == 0xDEAD0000u &&
              cpu.m_gpr[3].m_lo == 0x33u,
              "map zero initial PC to the ELF entry function");
    }

    // An unknown initial PC is returned untouched to the outer dispatcher.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("known", 0x00300000u, {0x00300000u}));
        const ee::t_ee_translated_entry entries[] = {dispatch_loop_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0xDEAD0000u;
        cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 0u &&
              result.guest_pc == 0xDEAD0000u,
              "return unknown initial PC to outer dispatcher");
    }

    // The dispatch budget prevents a malformed CFG or a guest loop from
    // monopolising the host thread indefinitely.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("loop", 0x00300000u, {0x00300000u}));
        const ee::t_ee_translated_entry entries[] = {dispatch_loop_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00300000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 5u);

        check(result.status == e_ee_dispatch_status::DISPATCH_LIMIT &&
              result.exit_reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) &&
              result.dispatch_count == 5u &&
              result.guest_pc == 0x00300000u,
              "stop dispatcher at configured budget");
    }

    // Metadata and native entry arrays must remain index-aligned.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("mismatch", 0x00400000u, {0x00400000u}));
        const ee::t_ee_translated_entry entries[] = {dispatch_loop_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 0u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00400000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(result.status == e_ee_dispatch_status::INVALID_ARGUMENT &&
              result.dispatch_count == 0u,
              "reject mismatched dispatcher metadata and code arrays");
    }

    std::printf("dispatcher_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
