#include "ee_decoder.h"
#include "ee_state.h"
#include "ee_runtime.h"
#include "ee_memory.h"
#include "ee_exceptions.h"
#include "instruction_lifter.h"
#include "native_codegen.h"
#include <map>
#include <string>

int main()
{
    ee::s_ee_cpu_state cpu;
    cpu.m_pc = 0x00100000u;
    cpu.m_gpr[31].m_lo = 0xDEAD0000u;
    return 0;
}

