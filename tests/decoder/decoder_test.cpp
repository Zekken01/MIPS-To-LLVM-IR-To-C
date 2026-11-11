// Decoder regression test for the MMI classification and the VALID/UNSUPPORTED
// contract. Words are composed from fields so each row documents the encoding.
#include "ee_decoder.h"
#include "instruction_lifter.h"
#include <cstdio>
#include <cstring>
using namespace ee;
static int g_pass = 0, g_fail = 0;
static uint32_t R(uint32_t op, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct)
{ return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct; }

static void expect(const char* what, uint32_t word, const char* mn, e_ee_valid v)
{
    s_ee_instruction i = decode(0x100000, word);
    bool ok = std::strcmp(i.mnemonic, mn) == 0 && i.valid == v;
    if (ok) ++g_pass; else { ++g_fail;
        std::printf("FAIL %-10s %08x got=%s/%d want=%s/%d\n", what, word, i.mnemonic, (int)i.valid, mn, (int)v); }
}

int main()
{
    const auto V = e_ee_valid::VALID, U = e_ee_valid::UNSUPPORTED, X = e_ee_valid::INVALID;
    // MMI groups: funct 0x08=MMI0, 0x28=MMI1, 0x09=MMI2, 0x29=MMI3; sub-op in sa.
    expect("paddw",   R(0x1c,4,5,6,0x00,0x08), "paddw",  V);
    expect("psubh",   R(0x1c,4,5,6,0x05,0x08), "psubh",  V);
    expect("pmaxh",   R(0x1c,4,5,6,0x07,0x08), "pmaxh",  V);
    expect("paddb",   R(0x1c,4,5,6,0x08,0x08), "paddb",  V);
    expect("pand",    R(0x1c,4,5,6,0x12,0x09), "pand",   V);
    expect("pxor",    R(0x1c,4,5,6,0x13,0x09), "pxor",   V);
    expect("por",     R(0x1c,4,5,6,0x12,0x29), "por",    V);
    expect("pnor",    R(0x1c,4,5,6,0x13,0x29), "pnor",   V);
    expect("mfhi1",   R(0x1c,0,0,6,0,0x10),    "mfhi1",  V);
    expect("mtlo1",   R(0x1c,4,0,0,0,0x13),    "mtlo1",  V);
    // Real ops that are NOT lowered exactly must never be VALID (previously a
    // silent fallback add/and/or/move was emitted for them).
    expect("paddsw",  R(0x1c,4,5,6,0x10,0x08), "paddsw", U);   // saturating: was plain add
    expect("pextlw",  R(0x1c,4,5,6,0x12,0x08), "pextlw", U);   // was lowered as OR
    expect("pcpyld",  R(0x1c,4,5,6,0x0e,0x09), "pcpyld", U);   // was lowered as AND
    expect("padduw",  R(0x1c,0,0,0,0x10,0x28), "padduw", U);   // the game's entry-point word 0x70000c28
    expect("pminw",   R(0x1c,4,5,6,0x03,0x28), "pminw",  U);
    expect("madd",    R(0x1c,4,5,6,0,0x00),    "madd",   V);
    expect("maddu",   R(0x1c,4,5,6,0,0x01),    "maddu",  V);
    expect("madd1",   R(0x1c,4,5,6,0,0x20),    "madd1",  V);
    expect("maddu1",  R(0x1c,4,5,6,0,0x21),    "maddu1", V);
    expect("madd bad sa", R(0x1c,4,5,6,1,0x00), "madd", U);
    {
        const auto madd = decode(0x100000, R(0x1c,4,5,6,0,0x00));
        const auto maddu = decode(0x100000, R(0x1c,4,5,6,0,0x01));
        const auto madd1 = decode(0x100000, R(0x1c,4,5,6,0,0x20));
        const auto maddu1 = decode(0x100000, R(0x1c,4,5,6,0,0x21));
        const auto mult1 = decode(0x100000, R(0x1c,4,5,6,0,0x18));
        const auto multu1 = decode(0x100000, R(0x1c,4,5,6,0,0x19));
        const bool ok = lift(madd).kind == LOP_MADD &&
                        lift(maddu).kind == LOP_MADDU &&
                        lift(madd1).kind == LOP_MADD1 &&
                        lift(maddu1).kind == LOP_MADDU1 &&
                        lift(mult1).kind == LOP_MULT1 &&
                        lift(multu1).kind == LOP_MULTU1;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL MADD/MADDU/MULT1 pipeline lift mapping\n");
    }
    expect("mult1",   R(0x1c,4,5,6,0,0x18),    "mult1",  V);
    expect("multu1",  R(0x1c,4,5,6,0,0x19),    "multu1", V);
    expect("mult1 bad sa", R(0x1c,4,5,6,1,0x18), "mult1", U);
    expect("pmfhl",   R(0x1c,0,0,6,0,0x30),    "pmfhl",  U);
    expect("psllh",   R(0x1c,0,5,6,3,0x34),    "psllh",  U);
    expect("reserved",R(0x1c,0,0,0,0x1f,0x28), "reserved", X);
    // COP2 field decoding is hierarchical: rs selects transfer, BC2, or VU0
    // macro mode; the low funct/sa fields are only interpreted inside macro mode.
    expect("qmfc2",   R(0x12,1,4,6,0,0), "qmfc2", U);
    expect("cfc2",    R(0x12,2,4,6,0,0), "cfc2", U);
    expect("qmtc2",   R(0x12,5,4,6,0,0), "qmtc2", U);
    expect("ctc2",    R(0x12,6,4,6,0,0), "ctc2", U);
    expect("vadd",    R(0x12,0x10,4,5,0,0x28), "vu0_macro", U);
    expect("vadda",   R(0x12,0x10,4,5,0,0x3c), "vu0_macro", U); // SPECIAL2 index 0
    expect("reserved macro", R(0x12,0x10,4,5,0,0x33), "cop2_reserved", X);
    expect("reserved special2", R(0x12,0x10,4,5,20,0x3c), "cop2_reserved", X); // index 80
    expect("reserved rs", R(0x12,0,4,5,0,0), "cop2_reserved", X);
    expect("reserved class", R(0x12,9,4,5,0,0), "cop2_reserved", X);
    {
        const auto b = decode(0x100000, R(0x12,8,0,0,0,0x02));
        const bool ok = b.valid == U && b.flow == e_ee_flow::FLOW_BRANCH &&
                        b.has_delay_slot && !b.branch_likely &&
                        b.branch_target == 0x0010000c &&
                        std::strcmp(b.mnemonic, "bc2f") == 0;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL bc2f flow/target decode\n");
    }
    {
        const auto b = decode(0x100000, R(0x12,8,2,0,0,0x02));
        const bool ok = b.valid == U && b.flow == e_ee_flow::FLOW_BRANCH_LIKELY &&
                        b.has_delay_slot && b.branch_likely &&
                        std::strcmp(b.mnemonic, "bc2fl") == 0;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL bc2fl likely decode\n");
    }
    // The BC2 table has 32 entries, but only rt=0..3 are defined. Do not
    // silently alias higher rt values through rt & 3.
    expect("bc2 rt=4",  R(0x12,8,4,0,0,0x02), "cop2_reserved", X);
    expect("bc2 rt=31", R(0x12,8,31,0,0,0x02), "cop2_reserved", X);

    // Exhaustively check the published SPECIAL1/SPECIAL2 dispatch tables.
    // Reserved encodings must stay INVALID; recognized operations remain
    // UNSUPPORTED until a complete VU0 lowering exists.
    for (uint32_t funct = 0; funct < 64; ++funct)
    {
        const bool defined = funct <= 50 || funct == 52 || funct == 53 ||
                             funct == 56 || funct == 57 || funct >= 60;
        const auto ins = decode(0x100000, R(0x12,0x10,0,0,0,funct));
        const auto want = defined ? U : X;
        const bool ok = ins.valid == want;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL COP2 SPECIAL1 funct=%02x valid=%u want=%u\n",
                             funct, (unsigned)ins.valid, (unsigned)want);
    }
    for (uint32_t index = 0; index < 128; ++index)
    {
        const bool defined = index <= 42 || (index >= 44 && index <= 49) ||
                             (index >= 52 && index <= 67);
        const uint32_t funct = 0x3c + (index & 3);
        const uint32_t sa = index >> 2;
        const auto ins = decode(0x100000, R(0x12,0x10,0,0,sa,funct));
        const auto want = defined ? U : X;
        const bool ok = ins.valid == want;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL COP2 SPECIAL2 index=%02x valid=%u want=%u\n",
                             index, (unsigned)ins.valid, (unsigned)want);
    }
    {
        const auto l = decode(0x100000, R(0x36,4,5,0,0,0));
        const auto st = decode(0x100000, R(0x3e,4,5,0,0,0));
        const bool ok = l.opcode == e_ee_opcode::OP_LQC2 &&
                        st.opcode == e_ee_opcode::OP_SQC2 &&
                        lift(l).kind == LOP_LQC2 && lift(st).kind == LOP_SQC2;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL lqc2/sqc2 primary opcode decode/lift\n");
    }
    {
        const auto m = decode(0x100000, R(0x12,1,4,6,0,0));
        const bool ok = lift(m).kind == LOP_UNSUPPORTED;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL qmfc2 must not silently lower to MFC2\n");
    }

    // COP1/COP0 validity is an emitter contract, not a claim that every
    // reserved or merely decoded coprocessor operation has been lowered.
    expect("mfc1", R(0x11,0x00,4,5,0,0), "cop1_transfer", V);
    expect("mtc1", R(0x11,0x04,4,5,0,0), "cop1_transfer", V);
    expect("cfc1 fcsr", R(0x11,0x02,4,31,0,0), "cop1_fcsr", V);
    expect("ctc1 fcsr", R(0x11,0x06,4,31,0,0), "cop1_fcsr", V);
    expect("cfc1 other", R(0x11,0x02,4,0,0,0), "cop1_control", U);
    expect("add.s", R(0x11,0x10,5,6,7,0x00), "cop1_s", V);
    expect("rsqrt.s", R(0x11,0x10,5,6,7,0x16), "cop1_s", V);
    expect("madda.s", R(0x11,0x10,5,6,7,0x1e), "cop1_s", V);
    expect("max.s", R(0x11,0x10,5,6,7,0x28), "cop1_s", U);
    expect("c.eq.s", R(0x11,0x10,5,6,7,0x32), "cop1_s", U);
    expect("cvt.w.s", R(0x11,0x10,5,6,7,0x24), "cop1_s", U);
    expect("unknown.s", R(0x11,0x10,5,6,7,0x08), "cop1_reserved", X);
    expect("cvt.s.w", R(0x11,0x14,0,6,7,0x20), "cvt_s_w", V);
    expect("bad.w", R(0x11,0x14,0,6,7,0x21), "cop1_reserved", X);
    expect("fmt.d", R(0x11,0x11,5,6,7,0x00), "cop1_reserved", X);
    expect("fmt.l", R(0x11,0x15,5,6,7,0x00), "cop1_reserved", X);
    expect("bc1 rt=4", R(0x11,0x08,4,0,0,0x00), "cop1_reserved", X);

    // Operations with overflow/trap or unimplemented comparison semantics
    // must not be advertised as supported just because their opcode is known.
    expect("addi overflow", R(0x08,4,5,0,0,0), "addi", U);
    expect("daddi overflow", R(0x18,4,5,0,0,0), "daddi", U);
    expect("slti", R(0x0a,4,5,0,0,0), "slti", U);
    expect("sltiu", R(0x0b,4,5,0,0,0), "sltiu", U);
    expect("add overflow", R(0x00,4,5,6,0,0x20), "signed_arith", U);
    expect("sub overflow", R(0x00,4,5,6,0,0x22), "signed_arith", U);
    expect("dadd overflow", R(0x00,4,5,6,0,0x2c), "signed_arith", U);
    expect("slt", R(0x00,4,5,6,0,0x2a), "r_compare", U);
    expect("tge", R(0x00,4,5,0,0,0x30), "trap", U);
    expect("bltzal", R(0x01,4,0x10,0,0,0), "bltzal", U);
    expect("bgezall", R(0x01,4,0x13,0,0,0), "bgezall", U);
    expect("tgei", R(0x01,4,0x08,0,0,0), "regimm_trap", U);
    expect("mtsab", R(0x01,4,0x18,0,0,0), "mtsab", U);
    expect("mtsah", R(0x01,4,0x19,0,0,0), "mtsah", U);
    expect("reserved regimm", R(0x01,4,0x1f,0,0,0), "regimm_reserved", X);
    {
        const auto jr_ra = decode(0x100000, R(0x00,31,0,0,0,0x08));
        const auto jr_v0 = decode(0x100000, R(0x00,2,0,0,0,0x08));
        const auto jalr = decode(0x100000, R(0x00,2,0,16,0,0x09));
        const bool ok = jr_ra.valid == V && jr_ra.flow == e_ee_flow::FLOW_RETURN &&
                        jr_v0.valid == U && jr_v0.flow == e_ee_flow::FLOW_JUMP &&
                        jalr.valid == U && jalr.flow == e_ee_flow::FLOW_CALL;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL indirect JR/JALR must fail closed without a dispatcher\n");
    }

    expect("mfc0", R(0x10,0x00,4,12,0,0), "mfc0", V);
    expect("mtc0", R(0x10,0x04,4,12,0,0), "mtc0", V);
    expect("tlbwi", R(0x10,0x10,0,0,0,0x02), "tlb", U);
    expect("bad cop0 co", R(0x10,0x10,0,0,0,0x3f), "cop0_reserved", X);
    expect("bc0", R(0x10,0x08,0,0,0,0x00), "bc0", U);
    {
        const auto eret = decode(0x100000, R(0x10,0x10,0,0,0,0x18));
        const bool ok = eret.valid == V && eret.flow == e_ee_flow::FLOW_RETURN &&
                        !eret.has_delay_slot;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL eret no-delay flow\n");
    }
    {
        const auto syscall = decode(0x100000, R(0x00,0,0,0,0,0x0c));
        const auto brk = decode(0x100000, R(0x00,0,0,0,0,0x0d));
        const bool ok = syscall.valid == V && brk.valid == V &&
                        syscall.flow == e_ee_flow::FLOW_EXCEPTION &&
                        brk.flow == e_ee_flow::FLOW_EXCEPTION;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL syscall/break exception flow\n");
    }

    {   // the game's entry word must classify as padduw, not VALID
        s_ee_instruction i = decode(0x100008, 0x70000c28);
        bool ok = std::strcmp(i.mnemonic, "padduw") == 0 && i.valid == U;
        ok ? ++g_pass : ++g_fail; if (!ok) std::printf("FAIL entry word\n");
    }
    std::printf("decoder_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
