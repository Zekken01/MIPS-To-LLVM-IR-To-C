#include "ee_decoder.h"
#include "ee_instruction.h"
#include "ee_opcode.h"
#include "function_analyzer.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool value, const char* name)
{
    if (value)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("FAIL: %s\n", name);
    }
}

uint32_t encode_i(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t immediate)
{
    return (opcode << 26) | (rs << 21) | (rt << 16) | immediate;
}

uint32_t encode_lui(uint32_t rt, uint16_t immediate)
{
    return encode_i(0x0f, 0, rt, immediate);
}

uint32_t encode_ori(uint32_t rs, uint32_t rt, uint16_t immediate)
{
    return encode_i(0x0d, rs, rt, immediate);
}

uint32_t encode_addiu(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x09, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_lw(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x23, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_beq(uint32_t rs, uint32_t rt, int16_t immediate)
{
    return encode_i(0x04, rs, rt, static_cast<uint16_t>(immediate));
}

uint32_t encode_jalr(uint32_t rs, uint32_t rd = 31)
{
    return (rs << 21) | (rd << 11) |
           static_cast<uint32_t>(ee::e_ee_opcode::FUNCT_JALR);
}

void add(std::map<uint32_t, ee::s_ee_instruction>& insns,
         uint32_t pc, uint32_t word)
{
    insns[pc] = ee::decode(pc, word);
}

std::set<uint32_t> recover(
    const std::map<uint32_t, ee::s_ee_instruction>& insns,
    uint32_t entry
)
{
    return ee::collect_static_jalr_targets(insns, {entry}, {entry});
}
} // namespace

int main()
{
    // Canonical absolute-address sequence: LUI + ORI + JALR.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1000, encode_lui(25, 0x0012));
        add(insns, 0x1004, encode_ori(25, 25, 0x3454));
        add(insns, 0x1008, encode_jalr(25));
        add(insns, 0x100c, 0); // delay slot
        const auto targets = recover(insns, 0x1000);
        check(targets == std::set<uint32_t>{0x00123454u},
              "recover LUI/ORI-built JALR target");
    }

    // Signed low-half address adjustment commonly used with %hi/%lo.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1100, encode_lui(25, 0x0013));
        add(insns, 0x1104, encode_addiu(25, 25, -4));
        add(insns, 0x1108, encode_jalr(25));
        add(insns, 0x110c, 0);
        const auto targets = recover(insns, 0x1100);
        check(targets == std::set<uint32_t>{0x0012fffcu},
              "recover LUI/ADDIU signed-low target");
    }

    // Constant $zero is a valid source for a small absolute address.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1200, encode_ori(0, 25, 0x1800));
        add(insns, 0x1204, encode_jalr(25));
        add(insns, 0x1208, 0);
        const auto targets = recover(insns, 0x1200);
        check(targets == std::set<uint32_t>{0x1800u},
              "propagate the architectural zero register");
    }

    // The target must be sampled at JALR, before its delay slot executes.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1300, encode_lui(25, 0x0010));
        add(insns, 0x1304, encode_ori(25, 25, 0x3000));
        add(insns, 0x1308, encode_jalr(25));
        add(insns, 0x130c, encode_ori(0, 25, 0x4000));
        const auto targets = recover(insns, 0x1300);
        check(targets == std::set<uint32_t>{0x00103000u},
              "capture JALR target before delay-slot register writes");
    }

    // A load into the target register destroys the static value.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1400, encode_lui(25, 0x0010));
        add(insns, 0x1404, encode_lw(8, 25, 0));
        add(insns, 0x1408, encode_jalr(25));
        add(insns, 0x140c, 0);
        const auto targets = recover(insns, 0x1400);
        check(targets.empty(),
              "do not infer target after an unknown memory load");
    }

    // Two CFG paths reaching JALR with different constants must merge to
    // unknown; seeing either individual path is not enough to report a target.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1000, encode_beq(1, 2, 5)); // target 0x1018
        add(insns, 0x1004, 0);
        add(insns, 0x1008, encode_lui(25, 0x0010));
        add(insns, 0x100c, encode_ori(25, 25, 0x1000));
        add(insns, 0x1010, encode_beq(0, 0, 3)); // target 0x1020
        add(insns, 0x1014, 0);
        add(insns, 0x1018, encode_lui(25, 0x0010));
        add(insns, 0x101c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1020, encode_jalr(25));
        add(insns, 0x1024, 0);
        const auto targets = recover(insns, 0x1000);
        check(targets.empty(),
              "discard JALR target when CFG predecessors disagree");
    }

    // Matching constants on every incoming path remain recoverable.
    {
        std::map<uint32_t, ee::s_ee_instruction> insns;
        add(insns, 0x1500, encode_beq(1, 2, 5)); // target 0x1518
        add(insns, 0x1504, 0);
        add(insns, 0x1508, encode_lui(25, 0x0010));
        add(insns, 0x150c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1510, encode_beq(0, 0, 3)); // target 0x1520
        add(insns, 0x1514, 0);
        add(insns, 0x1518, encode_lui(25, 0x0010));
        add(insns, 0x151c, encode_ori(25, 25, 0x2000));
        add(insns, 0x1520, encode_jalr(25));
        add(insns, 0x1524, 0);
        const auto targets = recover(insns, 0x1500);
        check(targets == std::set<uint32_t>{0x00102000u},
              "retain JALR target when all CFG paths agree");
    }

    std::printf("function_discovery_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
