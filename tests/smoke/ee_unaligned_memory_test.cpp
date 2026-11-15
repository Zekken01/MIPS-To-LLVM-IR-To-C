#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>

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

    std::puts("EE unaligned R5900 memory merge helpers: PASS");
    return 0;
}
