#pragma once
#include <cstdint>

namespace ee {

// R5900 opcodes (primary 6-bit opcode field) and SPECIAL function codes.
enum class e_ee_opcode : uint16_t
{
    OP_INVALID = 0xff,

    // MIPS I / III primary opcodes (R5900)
    OP_SPECIAL = 0x00,
    OP_REGIMM  = 0x01,
    OP_J       = 0x02,
    OP_JAL     = 0x03,
    OP_BEQ     = 0x04,
    OP_BNE     = 0x05,
    OP_BLEZ    = 0x06,
    OP_BGTZ    = 0x07,
    OP_ADDI    = 0x08,
    OP_ADDIU   = 0x09,
    OP_SLTI    = 0x0a,
    OP_SLTIU   = 0x0b,
    OP_ANDI    = 0x0c,
    OP_ORI     = 0x0d,
    OP_XORI    = 0x0e,
    OP_LUI     = 0x0f,
    // Branch-likely (MIPS II+)
    OP_BEQL    = 0x14,
    OP_BNEL    = 0x15,
    OP_BLEZL   = 0x16,
    OP_BGTZL   = 0x17,
    // 64-bit immediate arith
    OP_DADDI   = 0x18,
    OP_DADDIU  = 0x19,
    // Unaligned 64-bit partials
    OP_LDL     = 0x1a,
    OP_LDR     = 0x1b,
    OP_LQ      = 0x1e,   // load quadword (EE)
    OP_SQ      = 0x1f,   // store quadword (EE)
    OP_LB      = 0x20,
    OP_LH      = 0x21,
    OP_LWL     = 0x22,
    OP_LW      = 0x23,
    OP_LBU     = 0x24,
    OP_LHU     = 0x25,
    OP_LWR     = 0x26,
    OP_LWU     = 0x27,
    OP_SB      = 0x28,
    OP_SH      = 0x29,
    OP_SWL     = 0x2a,
    OP_SW      = 0x2b,
    OP_SDL     = 0x2c,
    OP_SDR     = 0x2d,
    OP_SWR     = 0x2e,
    OP_CACHE   = 0x2f,
    OP_LWC1    = 0x31,   // load word to COP1
    OP_PREF    = 0x33,
    OP_LDC1    = 0x35,
    OP_LQC2    = 0x36,   // load quadword to VU0 vector register
    OP_LD      = 0x37,   // load doubleword
    OP_SWC1    = 0x39,   // store word from COP1
    OP_SDC1    = 0x3d,
    OP_SQC2    = 0x3e,   // store quadword from VU0 vector register
    OP_SD      = 0x3f,   // store doubleword

    // Coprocessor primary opcodes.
    OP_COP0    = 0x10,
    OP_COP1    = 0x11,
    OP_COP2    = 0x12,

    // COP0 rs/fmt field (bits 25-21).
    COP0_MFC0  = 0x00,   // move from COP0
    COP0_MTC0  = 0x04,   // move to COP0
    COP0_BC0   = 0x08,   // branch on COP0 condition (rare)
    COP0_CO    = 0x10,   // CO bit set: TLB / ERET / EI / DI (funct selects)

    // COP0 funct when rs/fmt == COP0_CO (bit 25 set).
    COP0_TLBR  = 0x01,
    COP0_TLBWI = 0x02,
    COP0_TLBWR = 0x06,
    COP0_TLBP  = 0x08,
    COP0_ERET  = 0x18,   // exception return
    COP0_EI    = 0x38,   // enable interrupts (R5900)
    COP0_DI    = 0x39,   // disable interrupts (R5900)

    // COP1 sub-opcodes (bits 25-21 = COP1 function).
    COP1_MFC1  = 0x00,
    COP1_CFC1  = 0x02,
    COP1_MTC1  = 0x04,
    COP1_CTC1  = 0x06,
    COP1_BC1T  = 0x08,
    COP1_BC1F  = 0x09,
    COP1_BC1TL = 0x0a,
    COP1_BC1FL = 0x0b,
    COP1_LWC1  = 0x20,
    COP1_LDC1  = 0x21,
    COP1_LWL1  = 0x22,
    COP1_LWR1  = 0x23,
    COP1_SWC1  = 0x28,
    COP1_SDC1  = 0x29,
    COP1_SWL1  = 0x2a,
    COP1_SWR1  = 0x2b,

    // COP1.S funct codes (PCSX2 tbl_COP1_S / EE User's Manual).
    FUNCT_ADD_S     = 0x00,
    FUNCT_SUB_S     = 0x01,
    FUNCT_MUL_S     = 0x02,
    FUNCT_DIV_S     = 0x03,
    FUNCT_SQRT_S    = 0x04,
    FUNCT_ABS_S     = 0x05,
    FUNCT_MOV_S     = 0x06,
    FUNCT_NEG_S     = 0x07,
    FUNCT_RSQRT_S   = 0x16,
    FUNCT_ADDA_S    = 0x18,
    FUNCT_SUBA_S    = 0x19,
    FUNCT_MULA_S    = 0x1a,
    FUNCT_MADD_S    = 0x1c,
    FUNCT_MSUB_S    = 0x1d,
    FUNCT_MADDA_S   = 0x1e,
    FUNCT_MSUBA_S   = 0x1f,
    FUNCT_CVT_S_W   = 0x20,
    FUNCT_CVT_W_S   = 0x24,
    FUNCT_MAX_S     = 0x28,
    FUNCT_MIN_S     = 0x29,
    FUNCT_C_F_S     = 0x30,
    FUNCT_C_EQ_S    = 0x32,
    FUNCT_C_LT_S    = 0x34,
    FUNCT_C_LE_S    = 0x36,
    FUNCT_ROUND_LS  = 0x0c,
    FUNCT_TRUNC_LS  = 0x0d,
    FUNCT_CEIL_LS   = 0x0e,
    FUNCT_FLOOR_LS  = 0x0f,
    FUNCT_ROUND_WS  = 0x0c,
    FUNCT_TRUNC_WS  = 0x0d,
    FUNCT_CEIL_WS   = 0x0e,
    FUNCT_FLOOR_WS  = 0x0f,
    FUNCT_CVT_S_D   = 0x21,
    FUNCT_CVT_D_S   = 0x20,

    // COP2 VU0 primary rs-field selectors, cross-checked against the
    // R5900 interpreter opcode table. QMFC2/QMTC2 move 128-bit values and
    // have native runtime helper lowering; other macro selectors are gated
    // by the decoder's exact SPECIAL1/SPECIAL2 support table.
    COP2_QMFC2 = 0x01,
    COP2_CFC2  = 0x02,
    COP2_QMTC2 = 0x05,
    COP2_CTC2  = 0x06,
    COP2_BC2   = 0x08,

    // REGIMM rt sub-opcodes (when opcode == OP_REGIMM)
    REGIMM_BLTZ    = 0x00,
    REGIMM_BGEZ    = 0x01,
    REGIMM_BLTZL   = 0x02,
    REGIMM_BGEZL   = 0x03,
    REGIMM_TGEI    = 0x08,
    REGIMM_TGEIU   = 0x09,
    REGIMM_TLTI    = 0x0a,
    REGIMM_TLTIU   = 0x0b,
    REGIMM_TEQI    = 0x0c,
    REGIMM_TNEI    = 0x0e,
    REGIMM_BLTZAL  = 0x10,
    REGIMM_BGEZAL  = 0x11,
    REGIMM_BLTZALL = 0x12,
    REGIMM_BGEZALL = 0x13,
    REGIMM_MTSAB  = 0x18, // move to shift amount byte
    REGIMM_MTSAH  = 0x19, // move to shift amount halfword

    // SPECIAL function codes (R5900 / MIPS III, opcode == OP_SPECIAL)
    FUNCT_SLL      = 0x00,
    FUNCT_SRL      = 0x02,
    FUNCT_SRA      = 0x03,
    FUNCT_SLLV     = 0x04,
    FUNCT_ROTR     = 0x04, // alias; R5900 uses SLLV at this code
    FUNCT_SRLV     = 0x06,
    FUNCT_ROTRV    = 0x06, // alias
    FUNCT_SRAV     = 0x07,
    FUNCT_JR       = 0x08,
    FUNCT_JALR     = 0x09,
    FUNCT_MOVZ     = 0x0a,
    FUNCT_MOVN     = 0x0b,
    FUNCT_MOVNI    = 0x0b, // alias
    FUNCT_SYSCALL  = 0x0c,
    FUNCT_BREAK    = 0x0d,
    FUNCT_SYNC     = 0x0f,
    FUNCT_MFHI     = 0x10,
    FUNCT_MTHI     = 0x11,
    FUNCT_MFLO     = 0x12,
    FUNCT_MTLO     = 0x13,
    FUNCT_DSLLV    = 0x14,
    FUNCT_DSRLV    = 0x16,
    FUNCT_DSRAV    = 0x17,
    FUNCT_MULT     = 0x18,
    FUNCT_MULTU    = 0x19,
    FUNCT_DIV      = 0x1a,
    FUNCT_DIVU     = 0x1b,
    FUNCT_ADD      = 0x20,
    FUNCT_ADDU     = 0x21,
    FUNCT_SUB      = 0x22,
    FUNCT_SUBU     = 0x23,
    FUNCT_AND      = 0x24,
    FUNCT_OR       = 0x25,
    FUNCT_XOR      = 0x26,
    FUNCT_NOR      = 0x27,
    FUNCT_MFSA     = 0x28, // read R5900 shift amount register
    FUNCT_MTSA     = 0x29, // write R5900 shift amount register
    FUNCT_SLT      = 0x2a,
    FUNCT_SLTU     = 0x2b,
    FUNCT_DADD     = 0x2c,
    FUNCT_DADDU    = 0x2d,
    FUNCT_DSUB     = 0x2e,
    FUNCT_DSUBU    = 0x2f,
    FUNCT_TGE      = 0x30,
    FUNCT_TGEU     = 0x31,
    FUNCT_TLT      = 0x32,
    FUNCT_TLTU     = 0x33,
    FUNCT_TEQ      = 0x34,
    FUNCT_TNE      = 0x36,
    // 64-bit immediate shifts (MIPS III encodings)
    FUNCT_DSLL     = 0x38,
    FUNCT_DSRL     = 0x3a,
    FUNCT_DSRA     = 0x3b,
    FUNCT_DSLL32   = 0x3c,
    FUNCT_DSRL32   = 0x3e,
    FUNCT_DSRA32   = 0x3f,

    // MMI (multimedia/SIMD) primary opcode. R5900 MMI instructions use a
    // dedicated primary opcode with the function code selecting the sub-op and
    // lane width. All funct codes below fit in the 6-bit funct field (0-63).
    OP_MMI         = 0x1c,

    // MMI sub-opcodes (funct field when primary opcode == OP_MMI).
    // Packed add (wrap), one per lane width.
    FUNCT_MMI_ADD_B   = 0x10,
    FUNCT_MMI_ADD_H   = 0x11,
    FUNCT_MMI_ADD_W   = 0x12,
    FUNCT_MMI_ADD_D   = 0x13,
    // Packed subtract (wrap).
    FUNCT_MMI_SUB_B   = 0x14,
    FUNCT_MMI_SUB_H   = 0x15,
    FUNCT_MMI_SUB_W   = 0x16,
    FUNCT_MMI_SUB_D   = 0x17,
    // Saturating add (unsigned), byte/half/word.
    FUNCT_MMI_ADDU_SAT_B = 0x18,
    FUNCT_MMI_ADDU_SAT_H = 0x19,
    FUNCT_MMI_ADDU_SAT_W = 0x1a,
    // Packed logical (width-independent, full 128-bit).
    FUNCT_MMI_AND     = 0x20,
    FUNCT_MMI_OR      = 0x21,
    FUNCT_MMI_XOR     = 0x22,
    FUNCT_MMI_NOR     = 0x23,
    // Packed shifts by sa (full 128-bit).
    FUNCT_MMI_SLL     = 0x24,
    FUNCT_MMI_SRL     = 0x25,
    FUNCT_MMI_SRA     = 0x26,
    // Packed compare / min-max, byte/half/word.
    FUNCT_MMI_MAXU_B  = 0x30,
    FUNCT_MMI_MAXU_H  = 0x31,
    FUNCT_MMI_MAXU_W  = 0x32,
    FUNCT_MMI_MAXS_B  = 0x33,
    FUNCT_MMI_MAXS_H  = 0x34,
    FUNCT_MMI_MAXS_W  = 0x35,
    FUNCT_MMI_MINU_B  = 0x36,
    FUNCT_MMI_MINU_H  = 0x37,
    FUNCT_MMI_MINU_W  = 0x38,
    FUNCT_MMI_MINS_B  = 0x39,
    FUNCT_MMI_MINS_H  = 0x3a,
    FUNCT_MMI_MINS_W  = 0x3b,
    // Quadword shift right variable (shift amount in rs).
    FUNCT_MMI_QFSRV   = 0x3c,
    // Byte shuffle/permute (selector in rt).
    FUNCT_MMI_SHUFFLE = 0x3d,
    // Pack 2x-width lanes into width lanes with saturation.
    FUNCT_MMI_PACK    = 0x3e,

    OP_UNSUPPORTED = 0xffff
};

} // namespace ee
