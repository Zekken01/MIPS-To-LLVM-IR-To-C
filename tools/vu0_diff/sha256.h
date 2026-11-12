#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vu0_trace_fixture
{

inline uint32_t sha256_rotate_right(uint32_t value, uint32_t amount)
{
    return (value >> amount) | (value << (32u - amount));
}

// Small self-contained SHA-256 implementation for deterministic trace headers.
// The differential fixture is only 4 KB, so this avoids an external crypto
// dependency in both the recompiler tests and the pinned PCSX2 adapter.
inline std::string sha256_hex(const uint8_t* bytes, std::size_t byte_count)
{
    if (bytes == nullptr && byte_count != 0)
        return {};

    std::vector<uint8_t> message;
    if (byte_count != 0)
        message.assign(bytes, bytes + byte_count);

    const uint64_t bit_count = static_cast<uint64_t>(byte_count) * 8u;
    message.push_back(0x80u);
    while ((message.size() & 63u) != 56u)
        message.push_back(0);

    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<uint8_t>(bit_count >> shift));

    constexpr std::array<uint32_t, 64> k_round_constants =
    {{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    }};

    std::array<uint32_t, 8> hash =
    {{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    }};

    for (std::size_t block_offset = 0;
         block_offset < message.size();
         block_offset += 64u)
    {
        std::array<uint32_t, 64> words{};

        for (uint32_t i = 0; i < 16; ++i)
        {
            const std::size_t offset = block_offset + i * 4u;
            words[i] =
                (static_cast<uint32_t>(message[offset]) << 24) |
                (static_cast<uint32_t>(message[offset + 1]) << 16) |
                (static_cast<uint32_t>(message[offset + 2]) << 8) |
                static_cast<uint32_t>(message[offset + 3]);
        }

        for (uint32_t i = 16; i < 64; ++i)
        {
            const uint32_t x = words[i - 15];
            const uint32_t y = words[i - 2];
            const uint32_t s0 =
                sha256_rotate_right(x, 7) ^
                sha256_rotate_right(x, 18) ^
                (x >> 3);
            const uint32_t s1 =
                sha256_rotate_right(y, 17) ^
                sha256_rotate_right(y, 19) ^
                (y >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = hash[0];
        uint32_t b = hash[1];
        uint32_t c = hash[2];
        uint32_t d = hash[3];
        uint32_t e = hash[4];
        uint32_t f = hash[5];
        uint32_t g = hash[6];
        uint32_t h = hash[7];

        for (uint32_t i = 0; i < 64; ++i)
        {
            const uint32_t sum1 =
                sha256_rotate_right(e, 6) ^
                sha256_rotate_right(e, 11) ^
                sha256_rotate_right(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + sum1 + choose +
                                   k_round_constants[i] + words[i];
            const uint32_t sum0 =
                sha256_rotate_right(a, 2) ^
                sha256_rotate_right(a, 13) ^
                sha256_rotate_right(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    char output[65] = {};
    for (uint32_t i = 0; i < 8; ++i)
        std::snprintf(output + i * 8u, 9, "%08x", hash[i]);

    return std::string(output, 64);
}

} // namespace vu0_trace_fixture
