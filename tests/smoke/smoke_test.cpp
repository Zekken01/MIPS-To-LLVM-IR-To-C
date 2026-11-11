#include "ee_decoder.h"
#include "ee_instruction.h"
#include "instruction_lifter.h"

#include <cstdint>
#include <cstdio>

int main()
{
    // addiu $v0, $at, 7
    const uint32_t word = (0x09u << 26) | (1u << 21) | (2u << 16) | 7u;
    const ee::s_ee_instruction decoded = ee::decode(0x00100000u, word);

    if (decoded.valid != ee::e_ee_valid::VALID)
    {
        std::fprintf(stderr, "smoke: ADDIU decode failed\n");
        return 1;
    }

    const ee::s_lifted_op lifted = ee::lift(decoded);
    if (lifted.kind != ee::LOP_ADDIU ||
        lifted.rs != 1 ||
        lifted.rt != 2 ||
        lifted.immediate != 7)
    {
        std::fprintf(stderr, "smoke: ADDIU lift mismatch\n");
        return 1;
    }

    std::puts("PASS: ADDIU decode and lift");
    return 0;
}
