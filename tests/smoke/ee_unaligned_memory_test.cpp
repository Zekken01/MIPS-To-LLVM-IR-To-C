#include "ee_memory.h"
#include "ee_runtime.h"
#include "ee_decoder.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace
{
constexpr uint32_t k_base = 0x100u;
constexpr uint64_t k_old_gpr = 0xf123456789abcdefull;
constexpr uint64_t k_store_source = 0x123456789abcdef0ull;
constexpr uint8_t k_memory[8] = {0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

void seed(ee::ee_memory& memory)
{
    for (uint32_t i = 0; i < 8; ++i)
    {
        memory.write8(k_base + i, k_memory[i]);
    }
}

uint64_t load_le(const uint8_t* bytes, uint32_t count)
{
    uint64_t result = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        result |= static_cast<uint64_t>(bytes[i]) << (8u * i);
    }
    return result;
}

uint8_t byte_at(uint64_t value, uint32_t index)
{
    return static_cast<uint8_t>(value >> (8u * index));
}

bool check_memory(ee::ee_memory& memory, const uint8_t* expected, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (memory.read8(k_base + i) != expected[i])
        {
            return false;
        }
    }
    return true;
}

int fail(const char* instruction, uint32_t offset)
{
    std::fprintf(stderr, "unaligned %s at offset %u: FAIL\n", instruction, offset);
    return 1;
}
} // namespace

int main()
{
    ee::ee_memory memory(4096);
    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;

    for (uint32_t offset = 0; offset < 4; ++offset)
    {
        seed(memory);
        uint8_t word[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            word[i] = byte_at(k_old_gpr, i);
        }

        // LWL places the low (offset+1) memory bytes at the top of a word.
        for (uint32_t i = 0; i <= offset; ++i)
        {
            word[3u - offset + i] = k_memory[i];
        }
        const uint32_t low_lwl = static_cast<uint32_t>(load_le(word, 4));
        const uint64_t expect_lwl =
            static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(low_lwl)));
        if (ee::ee_mem_lwl(&runtime, k_base + offset, k_old_gpr) != expect_lwl)
        {
            return fail("LWL", offset);
        }

        for (uint32_t i = 0; i < 4; ++i)
        {
            word[i] = byte_at(k_old_gpr, i);
        }
        // LWR fills low bytes from memory, leaving unaffected destination
        // lanes alone (including the R5900's 32..63 preservation for partials).
        for (uint32_t i = 0; i < 4u - offset; ++i)
        {
            word[i] = k_memory[offset + i];
        }
        const uint32_t low_lwr = static_cast<uint32_t>(load_le(word, 4));
        const uint64_t expect_lwr = offset == 0u
            ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(low_lwr)))
            : ((k_old_gpr & 0xffffffff00000000ull) | low_lwr);
        if (ee::ee_mem_lwr(&runtime, k_base + offset, k_old_gpr) != expect_lwr)
        {
            return fail("LWR", offset);
        }

        seed(memory);
        uint8_t expected[8];
        for (uint32_t i = 0; i < 8; ++i)
        {
            expected[i] = k_memory[i];
        }
        // SWL changes low memory lanes using the high source bytes.
        for (uint32_t i = 0; i <= offset; ++i)
        {
            expected[i] = byte_at(k_store_source, 3u - offset + i);
        }
        ee::ee_mem_swl(&runtime, k_base + offset, k_store_source);
        if (!check_memory(memory, expected, 8))
        {
            return fail("SWL", offset);
        }

        seed(memory);
        for (uint32_t i = 0; i < 8; ++i)
        {
            expected[i] = k_memory[i];
        }
        for (uint32_t i = 0; i < 4u - offset; ++i)
        {
            expected[offset + i] = byte_at(k_store_source, i);
        }
        ee::ee_mem_swr(&runtime, k_base + offset, k_store_source);
        if (!check_memory(memory, expected, 8))
        {
            return fail("SWR", offset);
        }
    }

    for (uint32_t offset = 0; offset < 8; ++offset)
    {
        seed(memory);
        uint8_t result[8];
        for (uint32_t i = 0; i < 8; ++i)
        {
            result[i] = byte_at(k_old_gpr, i);
        }
        for (uint32_t i = 0; i <= offset; ++i)
        {
            result[7u - offset + i] = k_memory[i];
        }
        if (ee::ee_mem_ldl(&runtime, k_base + offset, k_old_gpr) != load_le(result, 8))
        {
            return fail("LDL", offset);
        }

        for (uint32_t i = 0; i < 8; ++i)
        {
            result[i] = byte_at(k_old_gpr, i);
        }
        for (uint32_t i = 0; i < 8u - offset; ++i)
        {
            result[i] = k_memory[offset + i];
        }
        if (ee::ee_mem_ldr(&runtime, k_base + offset, k_old_gpr) != load_le(result, 8))
        {
            return fail("LDR", offset);
        }

        seed(memory);
        uint8_t expected[8];
        for (uint32_t i = 0; i < 8; ++i)
        {
            expected[i] = k_memory[i];
        }
        for (uint32_t i = 0; i <= offset; ++i)
        {
            expected[i] = byte_at(k_store_source, 7u - offset + i);
        }
        ee::ee_mem_sdl(&runtime, k_base + offset, k_store_source);
        if (!check_memory(memory, expected, 8))
        {
            return fail("SDL", offset);
        }

        seed(memory);
        for (uint32_t i = 0; i < 8; ++i)
        {
            expected[i] = k_memory[i];
        }
        for (uint32_t i = 0; i < 8u - offset; ++i)
        {
            expected[offset + i] = byte_at(k_store_source, i);
        }
        ee::ee_mem_sdr(&runtime, k_base + offset, k_store_source);
        if (!check_memory(memory, expected, 8))
        {
            return fail("SDR", offset);
        }
    }


    // In addition to helper semantics, check that the LLVM CFG emitter
    // actually calls each exact helper instead of its old approximate
    // aligned LD/SD fallback. This fixture contains no proprietary code.
    constexpr uint32_t k_entry = 0x00100000u;
    const uint32_t opcodes[8] = {0x22u, 0x26u, 0x1au, 0x1bu,
                                 0x2au, 0x2eu, 0x2cu, 0x2du};
    const char* load_names[4] = {"lwl", "lwr", "ldl", "ldr"};
    const char* store_names[4] = {"swl", "swr", "sdl", "sdr"};
    std::map<uint32_t, ee::s_ee_instruction> instructions;
    for (uint32_t i = 0; i < 8; ++i)
    {
        const uint32_t pc = k_entry + i * 4u;
        const uint32_t word = (opcodes[i] << 26) | (8u << 21) | (9u << 16);
        const ee::s_ee_instruction ins = ee::decode(pc, word);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            return fail("decoder marked an unaligned instruction unsupported", i);
        }
        instructions.emplace(pc, ins);
    }
    const uint32_t jr_pc = k_entry + 8u * 4u;
    instructions.emplace(jr_pc, ee::decode(jr_pc, 0x03e00008u));
    instructions.emplace(jr_pc + 4u, ee::decode(jr_pc + 4u, 0u));

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(instructions, {k_entry});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        return fail("synthetic LLVM function discovery", 0);
    }

    for (int optimization = 0; optimization <= 3; ++optimization)
    {
        llvm::llvm_context context("unaligned_memory");
        llvm::module_builder builder(context, optimization);
        builder.emit_function_cfg(functions[0], instructions, "guest_unaligned_test");
        const std::string ir = context.ir();
        for (uint32_t i = 0; i < 4; ++i)
        {
            const std::string load_call =
                std::string("call i64 @ee_mem_") + load_names[i] + "(";
            const std::string store_call =
                std::string("call void @ee_mem_") + store_names[i] + "(";
            if (ir.find(load_call) == std::string::npos ||
                ir.find(store_call) == std::string::npos)
            {
                return fail("LLVM emitted a missing/approximate memory operation",
                            static_cast<uint32_t>(optimization));
            }
        }
    }

    std::puts("EE unaligned R5900 memory merge helpers: PASS");
    return 0;
}
