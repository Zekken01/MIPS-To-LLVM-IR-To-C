#include "ee_runtime.h"

namespace ee
{

e_ee_vu0_sync_mode ee_vu0_transfer_sync_mode(
    e_ee_vu0_transfer_direction direction,
    bool in_delay_slot
)
{
    if (!in_delay_slot)
    {
        return e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE;
    }

    return direction == e_ee_vu0_transfer_direction::FROM_VU0
        ? e_ee_vu0_sync_mode::FINISH_TO_E
        : e_ee_vu0_sync_mode::WAIT_TO_E_OR_M;
}

extern "C" uint32_t ee_vu0_sync(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || !rt ||
        mode > static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (!rt->m_vu0_state || !rt->m_vu0_sync)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const uint32_t result = rt->m_vu0_sync(
        rt->m_vu0_user, cpu, rt->m_vu0_state, mode, pc
    );

    if (result > static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    return result;
}

namespace
{
uint32_t validate_vu0_callback_result(uint32_t result)
{
    return result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
        ? result
        : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
}

bool vu0_is_running(const s_ee_vu0_state* vu0)
{
    return (vu0->m_vpu_status & 1u) != 0;
}
} // namespace

uint32_t ee_vu0_scheduler_sync_callback(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t mode,
    uint32_t pc
)
{
    if (!user || !cpu || !vu0 ||
        mode > static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    auto* scheduler = static_cast<s_ee_vu0_scheduler*>(user);
    if (!scheduler->m_execute)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const auto sync_mode = static_cast<e_ee_vu0_sync_mode>(mode);
    const uint32_t stop_at_e = EE_VU0_EXECUTE_STOP_AT_E;
    const uint32_t exact_cycles = EE_VU0_EXECUTE_EXACT_CYCLES;
    const uint32_t max_run_calls = scheduler->m_max_run_calls != 0
        ? scheduler->m_max_run_calls : 65536u;

    if (!vu0_is_running(vu0))
    {
        vu0->m_running = 0;
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    if (sync_mode == e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE)
    {
        // vu0Sync() is catch-up-only: VU0 ahead of the EE means no work.
        if (vu0->m_ee_cycle < vu0->m_cycle)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
        }

        const uint64_t cycle_budget = vu0->m_ee_cycle - vu0->m_cycle;
        const uint64_t previous_cycle = vu0->m_cycle;
        const uint32_t result = validate_vu0_callback_result(
            scheduler->m_execute(
                scheduler->m_user, cpu, vu0, cycle_budget,
                stop_at_e | exact_cycles, pc
            )
        );
        if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return result;
        }

        vu0->m_running = vu0_is_running(vu0) ? 1u : 0u;
        if (vu0_is_running(vu0) && cycle_budget != 0 &&
            vu0->m_cycle <= previous_cycle)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    // _vu0FinishMicro() and _vu0WaitMicro() both account the VU0 elapsed
    // cycles on the EE and then catch VU1 up. Require those host services
    // before executing anything so an absent service cannot cause a partial
    // synchronization followed by an apparent success.
    if (!scheduler->m_vu1_catch_up || !scheduler->m_schedule_event)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const uint64_t start_vu0_cycle = vu0->m_cycle;
    const uint64_t start_ee_cycle = vu0->m_ee_cycle;
    const bool wait_for_m = sync_mode == e_ee_vu0_sync_mode::WAIT_TO_E_OR_M;
    if (wait_for_m && vu0->m_m_bit != 0 &&
        vu0->m_ee_cycle <= vu0->m_cycle)
    {
        // PCSX2's M-bit fast path catches the EE up to VU0 and returns before
        // the normal VU1 catch-up/event-scheduling epilogue.
        vu0->m_ee_cycle = vu0->m_cycle;
        vu0->m_running = vu0_is_running(vu0) ? 1u : 0u;
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    const uint32_t execute_flags = stop_at_e |
        (wait_for_m ? EE_VU0_EXECUTE_STOP_AT_M : 0u);

    bool terminated = false;
    uint32_t run_calls = 0;
    while (run_calls < max_run_calls && vu0_is_running(vu0))
    {
        const uint64_t previous_cycle = vu0->m_cycle;
        const uint32_t result = validate_vu0_callback_result(
            scheduler->m_execute(
                scheduler->m_user, cpu, vu0, 0x7fffffffu,
                execute_flags, pc
            )
        );
        if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return result;
        }

        ++run_calls;
        vu0->m_running = vu0_is_running(vu0) ? 1u : 0u;
        if (!vu0_is_running(vu0))
        {
            terminated = true;
            break;
        }

        if (wait_for_m &&
            (vu0->m_m_bit != 0 || vu0->m_ee_cycle <= vu0->m_cycle))
        {
            terminated = true;
            break;
        }

        // A host engine that reports success without advancing or stopping
        // would otherwise make a malformed VU program spin forever here.
        if (vu0->m_cycle <= previous_cycle)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }
    }

    if (!terminated)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    // PCSX2's addCycles epilogue advances EE time by the amount of VU0
    // execution consumed in this call. Do not assign EE time to VU0 time:
    // VU0 may already have been ahead when the interlocked wait began.
    if (vu0->m_cycle < start_vu0_cycle)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }
    vu0->m_ee_cycle = start_ee_cycle + (vu0->m_cycle - start_vu0_cycle);

    const uint32_t vu1_result = validate_vu0_callback_result(
        scheduler->m_vu1_catch_up(scheduler->m_user, cpu, vu0, pc)
    );
    if (vu1_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return vu1_result;
    }

    if (vu0_is_running(vu0))
    {
        scheduler->m_schedule_event(
            scheduler->m_user, cpu, vu0, 4u, pc
        );
    }

    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

namespace
{
uint32_t read_le32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

e_ee_vu0_micro_opcode decode_lower_secondary(uint32_t word)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t secondary = word & 0x3fu;

    switch (secondary)
    {
    case 0x30: return op::IADD;
    case 0x31: return op::ISUB;
    case 0x32: return op::IADDI;
    case 0x34: return op::IAND;
    case 0x35: return op::IOR;
    default: break;
    }

    if (secondary < 0x3cu || secondary > 0x3fu)
    {
        return op::UNKNOWN;
    }

    const uint32_t table = secondary - 0x3cu;
    const uint32_t index = (word >> 6) & 0x1fu;

    switch (table)
    {
    case 0:
        switch (index)
        {
        case 12: return op::MOVE;
        case 13: return op::LQI;
        case 14: return op::DIV;
        case 15: return op::MTIR;
        case 16: return op::RNEXT;
        case 25: return op::MFP;
        case 26: return op::XTOP;
        case 27: return op::XGKICK;
        case 28: return op::ESADD;
        case 29: return op::EATAN_XY;
        case 30: return op::ESQRT;
        case 31: return op::ESIN;
        default: return op::UNKNOWN;
        }
    case 1:
        switch (index)
        {
        case 12: return op::MR32;
        case 13: return op::SQI;
        case 14: return op::SQRT;
        case 15: return op::MFIR;
        case 16: return op::RGET;
        case 26: return op::XITOP;
        case 28: return op::ERSADD;
        case 29: return op::EATAN_XZ;
        case 30: return op::ERSQRT;
        case 31: return op::EATAN;
        default: return op::UNKNOWN;
        }
    case 2:
        switch (index)
        {
        case 13: return op::LQD;
        case 14: return op::RSQRT;
        case 15: return op::ILWR;
        case 16: return op::RINIT;
        case 28: return op::ELENG;
        case 29: return op::ESUM;
        case 30: return op::ERCPR;
        case 31: return op::EEXP;
        default: return op::UNKNOWN;
        }
    case 3:
        switch (index)
        {
        case 13: return op::SQD;
        case 14: return op::WAITQ;
        case 15: return op::ISWR;
        case 16: return op::RXOR;
        case 28: return op::ERLENG;
        case 30: return op::WAITP;
        default: return op::UNKNOWN;
        }
    default:
        return op::UNKNOWN;
    }
}

e_ee_vu0_micro_opcode decode_lower_micro_opcode(uint32_t word)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t primary = word >> 25;

    switch (primary)
    {
    case 0x00: return op::LQ;
    case 0x01: return op::SQ;
    case 0x04: return op::ILW;
    case 0x05: return op::ISW;
    case 0x08: return op::IADDIU;
    case 0x09: return op::ISUBIU;

    case 0x10: return op::FCEQ;
    case 0x11: return op::FCSET;
    case 0x12: return op::FCAND;
    case 0x13: return op::FCOR;
    case 0x14: return op::FSEQ;
    case 0x15: return op::FSSET;
    case 0x16: return op::FSAND;
    case 0x17: return op::FSOR;
    case 0x18: return op::FMEQ;
    case 0x1a: return op::FMAND;
    case 0x1b: return op::FMOR;
    case 0x1c: return op::FCGET;

    case 0x20: return op::B;
    case 0x21: return op::BAL;
    case 0x24: return op::JR;
    case 0x25: return op::JALR;
    case 0x28: return op::IBEQ;
    case 0x29: return op::IBNE;
    case 0x2c: return op::IBLTZ;
    case 0x2d: return op::IBGTZ;
    case 0x2e: return op::IBLEZ;
    case 0x2f: return op::IBGEZ;

    case 0x3c:
        return decode_lower_secondary(word);

    default:
        return op::UNKNOWN;
    }
}

e_ee_vu0_micro_opcode decode_upper_secondary(
    uint32_t word,
    uint32_t table
)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t index = (word >> 6) & 0x1fu;

    switch (table)
    {
    case 0:
        switch (index)
        {
        case 0: return op::ADDA_X;
        case 1: return op::SUBA_X;
        case 2: return op::MADDA_X;
        case 3: return op::MSUBA_X;
        case 4: return op::ITOF0;
        case 5: return op::FTOI0;
        case 6: return op::MULA_X;
        case 7: return op::MULA_Q;
        case 8: return op::ADDA_Q;
        case 9: return op::SUBA_Q;
        case 10: return op::ADDA;
        case 11: return op::SUBA;
        default: return op::UNKNOWN;
        }
    case 1:
        switch (index)
        {
        case 0: return op::ADDA_Y;
        case 1: return op::SUBA_Y;
        case 2: return op::MADDA_Y;
        case 3: return op::MSUBA_Y;
        case 4: return op::ITOF4;
        case 5: return op::FTOI4;
        case 6: return op::MULA_Y;
        case 7: return op::ABS;
        case 8: return op::MADDA_Q;
        case 9: return op::MSUBA_Q;
        case 10: return op::MADDA;
        case 11: return op::MSUBA;
        default: return op::UNKNOWN;
        }
    case 2:
        switch (index)
        {
        case 0: return op::ADDA_Z;
        case 1: return op::SUBA_Z;
        case 2: return op::MADDA_Z;
        case 3: return op::MSUBA_Z;
        case 4: return op::ITOF12;
        case 5: return op::FTOI12;
        case 6: return op::MULA_Z;
        case 7: return op::MULA_I;
        case 8: return op::ADDA_I;
        case 9: return op::SUBA_I;
        case 10: return op::MULA;
        case 11: return op::OPMULA;
        default: return op::UNKNOWN;
        }
    case 3:
        switch (index)
        {
        case 0: return op::ADDA_W;
        case 1: return op::SUBA_W;
        case 2: return op::MADDA_W;
        case 3: return op::MSUBA_W;
        case 4: return op::ITOF15;
        case 5: return op::FTOI15;
        case 6: return op::MULA_W;
        case 7: return op::CLIP;
        case 8: return op::MADDA_I;
        case 9: return op::MSUBA_I;
        case 11: return op::NOP;
        default: return op::UNKNOWN;
        }
    default:
        return op::UNKNOWN;
    }
}

e_ee_vu0_micro_opcode decode_upper_micro_opcode(uint32_t word)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t primary = word & 0x3fu;

    if (primary >= 0x3cu)
    {
        return decode_upper_secondary(word, primary - 0x3cu);
    }

    switch (primary)
    {
    case 0x00: return op::ADD_X;
    case 0x01: return op::ADD_Y;
    case 0x02: return op::ADD_Z;
    case 0x03: return op::ADD_W;
    case 0x04: return op::SUB_X;
    case 0x05: return op::SUB_Y;
    case 0x06: return op::SUB_Z;
    case 0x07: return op::SUB_W;
    case 0x08: return op::MADD_X;
    case 0x09: return op::MADD_Y;
    case 0x0a: return op::MADD_Z;
    case 0x0b: return op::MADD_W;
    case 0x0c: return op::MSUB_X;
    case 0x0d: return op::MSUB_Y;
    case 0x0e: return op::MSUB_Z;
    case 0x0f: return op::MSUB_W;
    case 0x10: return op::MAX_X;
    case 0x11: return op::MAX_Y;
    case 0x12: return op::MAX_Z;
    case 0x13: return op::MAX_W;
    case 0x14: return op::MINI_X;
    case 0x15: return op::MINI_Y;
    case 0x16: return op::MINI_Z;
    case 0x17: return op::MINI_W;
    case 0x18: return op::MUL_X;
    case 0x19: return op::MUL_Y;
    case 0x1a: return op::MUL_Z;
    case 0x1b: return op::MUL_W;
    case 0x1c: return op::MUL_Q;
    case 0x1d: return op::MAX_I;
    case 0x1e: return op::MUL_I;
    case 0x1f: return op::MINI_I;
    case 0x20: return op::ADD_Q;
    case 0x21: return op::MADD_Q;
    case 0x22: return op::ADD_I;
    case 0x23: return op::MADD_I;
    case 0x24: return op::SUB_Q;
    case 0x25: return op::MSUB_Q;
    case 0x26: return op::SUB_I;
    case 0x27: return op::MSUB_I;
    case 0x28: return op::ADD;
    case 0x29: return op::MADD;
    case 0x2a: return op::MUL;
    case 0x2b: return op::MAX;
    case 0x2c: return op::SUB;
    case 0x2d: return op::MSUB;
    case 0x2e: return op::OPMSUB;
    case 0x2f: return op::MINI;
    default: return op::UNKNOWN;
    }
}

s_ee_vu0_micro_op_fields decode_micro_op_fields(
    uint32_t word,
    bool upper
)
{
    const uint8_t ft = static_cast<uint8_t>((word >> 16) & 0x1fu);
    const uint8_t fs = static_cast<uint8_t>((word >> 11) & 0x1fu);
    const uint8_t fd = static_cast<uint8_t>((word >> 6) & 0x1fu);

    return {
        static_cast<uint8_t>(upper ? (word & 0x3fu) : (word >> 25)),
        ft,
        fs,
        fd,
        static_cast<uint8_t>(ft & 0x0fu),
        static_cast<uint8_t>(fs & 0x0fu),
        static_cast<uint8_t>(fd & 0x0fu),
        static_cast<uint8_t>((word >> 21) & 0x0fu),
        upper ? decode_upper_micro_opcode(word) : decode_lower_micro_opcode(word)
    };
}} // namespace

extern "C" uint32_t ee_vu0_fetch_micro_instruction(
    s_ee_vu0_state* vu0,
    s_ee_vu0_micro_instruction* out
)
{
    if (!vu0 || !out)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t pc = vu0->m_micro_pc & EE_VU0_MICRO_MEMORY_MASK;
    if ((pc & 7u) != 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint8_t* const bytes = &vu0->m_micro_memory[pc];
    const uint32_t lower = read_le32(bytes);
    const uint32_t upper = read_le32(bytes + 4);

    uint32_t flags = 0;
    if ((upper & 0x80000000u) != 0) flags |= EE_VU0_MICRO_FLAG_I;
    if ((upper & 0x40000000u) != 0) flags |= EE_VU0_MICRO_FLAG_E;
    if ((upper & 0x20000000u) != 0) flags |= EE_VU0_MICRO_FLAG_M;
    if ((upper & 0x10000000u) != 0) flags |= EE_VU0_MICRO_FLAG_D;
    if ((upper & 0x08000000u) != 0) flags |= EE_VU0_MICRO_FLAG_T;

    const s_ee_vu0_micro_op_fields lower_fields =
        decode_micro_op_fields(lower, false);
    const s_ee_vu0_micro_op_fields upper_fields =
        decode_micro_op_fields(upper, true);

    *out = {
        pc, lower, upper, flags, lower_fields, upper_fields
    };
    vu0->m_micro_pc = (pc + 8u) & EE_VU0_MICRO_MEMORY_MASK;
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

extern "C" uint32_t ee_vu0_read_vi(
    const s_ee_vu0_state* vu0,
    uint32_t index
)
{
    if (!vu0 || index >= 32 || index == 0)
    {
        return 0;
    }

    switch (index)
    {
    case 16: return vu0->m_status_flags;
    case 17: return vu0->m_mac_flags;
    case 18: return vu0->m_clip_flags;
    case 26: return vu0->m_tpc;
    case 28: return vu0->m_fbrst;
    case 29: return vu0->m_vpu_status;
    default: return vu0->m_vi[index];
    }
}

extern "C" void ee_vu0_write_vi(
    s_ee_vu0_state* vu0,
    uint32_t index,
    uint32_t value
)
{
    if (!vu0 || index >= 32 || index == 0)
    {
        return;
    }

    vu0->m_vi[index] = value;
    switch (index)
    {
    case 16: vu0->m_status_flags = value; break;
    case 17: vu0->m_mac_flags = value; break;
    case 18: vu0->m_clip_flags = value; break;
    case 26: vu0->m_tpc = value; break;
    case 28: vu0->m_fbrst = value; break;
    case 29: vu0->m_vpu_status = value; break;
    default: break;
    }
}


namespace
{
constexpr uint32_t VU0_REG_R = 20;
constexpr uint32_t VU0_R_MASK = 0x007fffffu;
} // namespace

extern "C" uint32_t ee_vu0_qmfc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vf_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vf_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (gpr_index != 0)
    {
        // VF0 is architecturally constant: (0, 0, 0, 1.0f). Do not trust
        // the backing array for register zero, which must never be written.
        cpu->m_gpr[gpr_index] = vf_index == 0
            ? s_ee_u128{0, 0x3f80000000000000ULL}
            : cpu->m_vu0[vf_index];
    }

    return result;
}

extern "C" uint32_t ee_vu0_qmtc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vf_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vf_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (vf_index != 0)
    {
        cpu->m_vu0[vf_index] = gpr_index == 0
            ? s_ee_u128{}
            : cpu->m_gpr[gpr_index];
    }

    return result;
}

extern "C" uint32_t ee_vu0_cfc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vi_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vi_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (gpr_index != 0)
    {
        uint32_t value = ee_vu0_read_vi(rt->m_vu0_state, vi_index);
        if (vi_index == VU0_REG_R)
        {
            // PCSX2 writes only UL[0] on this special path. Preserve UL[1]
            // rather than sign-extending or zero-extending the 23-bit value.
            cpu->m_gpr[gpr_index].m_lo =
                (cpu->m_gpr[gpr_index].m_lo & 0xffffffff00000000ULL) |
                (static_cast<uint64_t>(value) & VU0_R_MASK);
        }
        else
        {
            // All ordinary VI reads sign-extend UL[0] into UL[1].
            cpu->m_gpr[gpr_index].m_lo =
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
        }
    }

    return result;
}


extern "C" uint32_t ee_vu0_ctc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vi_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vi_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t sync_result = ee_vu0_sync(cpu, rt, mode, pc);
    if (sync_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return sync_result;
    }

    if (vi_index == 0)
    {
        return sync_result;
    }

    // The source is the low 32-bit word; GPR0 is hardwired to zero even if a
    // malformed host state has non-zero backing storage for it.
    const uint32_t value = gpr_index == 0
        ? 0
        : static_cast<uint32_t>(cpu->m_gpr[gpr_index].m_lo);

    s_ee_vu0_state* vu0 = rt->m_vu0_state;
    switch (vi_index)
    {
    case 17: // MAC_FLAG is read-only.
    case 26: // TPC is read-only.
    case 29: // VPU_STAT is read-only.
        return sync_result;

    case 20: // R: preserve the implicit 1.0 exponent.
        ee_vu0_write_vi(vu0, vi_index, (value & VU0_R_MASK) | 0x3f800000u);
        return sync_result;

    case 28: // FBRST: mask stored bits and route reset/break actions to host.
    {
        constexpr uint32_t FBRST_ACTION_MASK = 0x00000303u;
        if ((value & FBRST_ACTION_MASK) != 0)
        {
            if (!rt->m_vu0_control)
            {
                return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
            }

            const uint32_t control_result = rt->m_vu0_control(
                rt->m_vu0_control_user, cpu, vu0, vi_index, value, pc
            );
            if (control_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
            {
                return control_result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
                    ? control_result
                    : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
            }
        }

        ee_vu0_write_vi(vu0, vi_index, value & 0x00000c0cu);
        return sync_result;
    }

    case 31: // CMSAR1 starts a VU1 micro subroutine after VU1 is finished.
    {
        if (!rt->m_vu0_control)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }

        const uint32_t control_result = rt->m_vu0_control(
            rt->m_vu0_control_user, cpu, vu0, vi_index, value & 0xffffu, pc
        );
        if (control_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return control_result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
                ? control_result
                : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }

        vu0->m_cmsar1 = value & 0xffffu;
        return sync_result;
    }

    case 18: // CLIP_FLAG also updates the separately tracked clip flags.
        vu0->m_clip_flags = value;
        ee_vu0_write_vi(vu0, vi_index, value);
        return sync_result;

    default:
        ee_vu0_write_vi(vu0, vi_index, value);
        return sync_result;
    }
}

extern "C" uint32_t ee_vu0_get_branch_condition(const s_ee_runtime* rt)
{
    if (!rt || !rt->m_vu0_state)
    {
        // 0 and 1 are valid branch predicates. 2 is the fail-closed sentinel.
        return 2;
    }

    // PCSX2's BC2 predicate is VU0.VI[REG_VPU_STAT].US[0] bit 8.
    return (rt->m_vu0_state->m_vpu_status & (1u << 8)) != 0 ? 1u : 0u;
}


} // namespace ee
