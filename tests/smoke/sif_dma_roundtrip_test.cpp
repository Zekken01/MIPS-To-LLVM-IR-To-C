#include "iop_sif0_dma_transport.h"
#include "iop_sif1_dma_transport.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
constexpr uint32_t k_ee_request = 0x00100000u;
constexpr uint32_t k_ee_reply = 0x00180000u;
constexpr uint32_t k_iop_request = 0x00018000u;
constexpr uint32_t k_iop_reply = 0x00040000u;
constexpr uint32_t k_iop_reply_tag = 0x00014000u;
constexpr uint32_t k_payload_bytes = 16u;

uint32_t encode_i(uint32_t opcode, uint32_t rs, uint32_t rt,
                  uint32_t immediate)
{
    return opcode << 26u | rs << 21u | rt << 16u |
           (immediate & 0xffffu);
}

uint32_t encode_r(uint32_t rs, uint32_t rt, uint32_t rd,
                  uint32_t funct)
{
    return rs << 21u | rt << 16u | rd << 11u | funct;
}

std::vector<uint8_t> make_rom()
{
    constexpr uint32_t t0 = 8u;
    constexpr uint32_t t1 = 9u;
    constexpr uint32_t t2 = 10u;
    constexpr uint32_t t3 = 11u;
    constexpr uint32_t t4 = 12u;
    constexpr uint32_t t5 = 13u;
    constexpr uint32_t t6 = 14u;
    constexpr uint32_t t7 = 15u;

    std::vector<uint32_t> program;
    const auto emit = [&](uint32_t opcode)
    {
        program.push_back(opcode);
    };
    const auto lui = [&](uint32_t rt, uint32_t imm)
    {
        emit(encode_i(0x0fu, 0u, rt, imm));
    };
    const auto ori = [&](uint32_t rt, uint32_t rs, uint32_t imm)
    {
        emit(encode_i(0x0du, rs, rt, imm));
    };
    const auto lw = [&](uint32_t rt, uint32_t rs, uint32_t off)
    {
        emit(encode_i(0x23u, rs, rt, off));
    };
    const auto sw = [&](uint32_t rt, uint32_t rs, uint32_t off)
    {
        emit(encode_i(0x2bu, rs, rt, off));
    };
    const auto logic_xor = [&](uint32_t rd, uint32_t rs, uint32_t rt)
    {
        emit(encode_r(rs, rt, rd, 0x26u));
    };

    // R3000 guest controls both DMA channels. The request/reply
    // buffers and tags are actual R3000 RAM. It waits for receiver
    // completion before consuming data or processing the next packet.
    lui(t0, 0xbf80u);
    ori(t0, t0, 0x1538u);  // DMA10 CHCR
    lui(t1, 0x0100u);
    ori(t1, t1, 0x0700u);  // DMA10 receiver start
    lui(t7, 0x0100u);      // STR bit
    sw(t1, t0, 0u);

    const size_t sif1_wait = program.size();
    lw(t6, t0, 0u);
    emit(encode_r(t6, t7, t6, 0x24u)); // and t6,t6,t7
    emit(encode_i(0x05u, t6, 0u, static_cast<uint32_t>(
        static_cast<int32_t>(sif1_wait) -
        static_cast<int32_t>(program.size()) - 1)));
    emit(0u); // branch delay slot

    lui(t2, 0x0001u);
    ori(t2, t2, 0x8000u);  // 0x00018000 IOP receive buffer
    lui(t3, 0x0004u);      // 0x00040000 reply buffer

    // Validate the payload checksum: word3 = word0 XOR word1 XOR word2.
    lw(t6, t2, 0u);
    lw(t5, t2, 4u);
    logic_xor(t6, t6, t5);
    lw(t5, t2, 8u);
    logic_xor(t6, t6, t5);
    lw(t5, t2, 12u);
    const size_t reject_branch = program.size();
    emit(0u); // patched to bne t6,t5,reject
    emit(0u);

    // Sequence survives unchanged; transform all three other words
    // in guest R3000 instructions, never by a host callback.
    lw(t6, t2, 0u);
    sw(t6, t3, 0u);
    lw(t6, t2, 4u);
    emit(encode_i(0x0eu, t6, t6, 0x5aa5u)); // xori
    sw(t6, t3, 4u);
    lw(t6, t2, 8u);
    emit(encode_i(0x09u, t6, t6, 7u)); // addiu
    sw(t6, t3, 8u);
    lw(t6, t2, 12u);
    emit(encode_r(t6, 0u, t6, 0x27u)); // nor
    sw(t6, t3, 12u);

    lui(t4, 0xbf80u);
    ori(t4, t4, 0x152cu); // DMA9 TADR
    lui(t5, 0x0001u);
    ori(t5, t5, 0x4000u); // 0x00014000 IOP DMA9 source tag
    sw(t5, t4, 0u);
    emit(encode_i(0x09u, t4, t4, 0xfffcu)); // addiu t4,-4 -> CHCR
    lui(t5, 0x0100u);
    ori(t5, t5, 0x0701u);
    sw(t5, t4, 0u);

    const size_t sif0_wait = program.size();
    lw(t6, t4, 0u);
    emit(encode_r(t6, t7, t6, 0x24u));
    emit(encode_i(0x05u, t6, 0u, static_cast<uint32_t>(
        static_cast<int32_t>(sif0_wait) -
        static_cast<int32_t>(program.size()) - 1)));
    emit(0u);

    // Reuse hardware registers and guest buffers for the next request.
    emit(0x08000000u | ((0xbfc00000u >> 2u) & 0x03ffffffu));
    emit(0u);
    const size_t reject_target = program.size();
    emit(0x0000000du); // breakpoint only for invalid guest checksum
    program[reject_branch] = encode_i(
        0x05u, t6, t5,
        static_cast<uint32_t>(
            static_cast<int32_t>(reject_target) -
            static_cast<int32_t>(reject_branch) - 1));

    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    for (uint32_t index = 0u; index < program.size(); ++index)
    {
        const uint32_t word = program[index];
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            rom[index * 4u + lane] =
                static_cast<uint8_t>(word >> (lane * 8u));
        }
    }
    return rom;
}

bool ee_write(ee::c_ee_dmac& dmac, uint32_t base,
              uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 word{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, base + offset, 4u, &word);
}

bool ee_read(ee::c_ee_dmac& dmac, uint32_t base,
             uint32_t offset, uint32_t* value)
{
    ee::s_ee_u128 word{};
    const bool valid = value != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &dmac, base + offset, 4u, &word);
    if (valid)
    {
        *value = static_cast<uint32_t>(word.m_lo);
    }
    return valid;
}

uint32_t checksum(uint32_t word)
{
    return (word & 0xffu) + ((word >> 8u) & 0xffu) +
           ((word >> 16u) & 0xffu) + ((word >> 24u) & 0xffu);
}

int fail(const char* reason, uint32_t quantum, uint32_t seq,
         uint32_t guest_pc)
{
    std::fprintf(stderr,
        "SIF roundtrip: %s (quantum=%u sequence=%u guest_pc=0x%08x)\n",
        reason, quantum, seq, guest_pc);
    return 1;
}

struct s_fixture
{
    ee::ee_memory m_ram{32u * 1024u * 1024u};
    ee::c_ee_dmac m_dmac{};
    iop::c_kernel m_iop{};
    std::unique_ptr<iop::c_sif1_dma_transport> m_sif1{};
    std::unique_ptr<iop::c_sif0_dma_transport> m_sif0{};

    s_fixture()
    {
        m_dmac.bind_memory(&m_ram);
        m_dmac.reset();
    }

    bool start()
    {
        if (!m_iop.start_rom0_boot(make_rom()))
        {
            return false;
        }
        m_sif1 = std::make_unique<iop::c_sif1_dma_transport>(
            m_ram, m_dmac, m_iop.cpu(), m_iop.hardware());
        m_sif0 = std::make_unique<iop::c_sif0_dma_transport>(
            m_ram, m_dmac, m_iop.cpu(), m_iop.hardware());

        // The guest DMA9 tag references the reply transformed by the
        // R3000 program, not a host-supplied response packet.
        const uint32_t tag[] =
        {
            0x80000000u | k_iop_reply,
            k_payload_bytes / 4u,
            0x90000001u, k_ee_reply, 0u, 0u
        };
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            if (!m_iop.cpu().write32(k_iop_reply_tag + i * 4u, tag[i]))
            {
                return false;
            }
        }
        return m_iop.hardware().write(
            iop::c_hw::k_i_mask, 4u,
            1u << iop::c_hw::k_dma_irq);
    }

    bool arm_ee_receiver()
    {
        return ee_write(
                   m_dmac, ee::c_ee_dmac::k_sif0_base,
                   0x20u, 0u) &&
               ee_write(
                   m_dmac, ee::c_ee_dmac::k_sif0_base,
                   0u, 0x184u);
    }

    bool send_request(uint32_t seq, uint32_t p1, uint32_t p2,
                      bool valid_checksum)
    {
        const uint32_t payload[4] =
        {
            seq, p1, p2,
            (seq ^ p1 ^ p2) ^ (valid_checksum ? 0u : 1u)
        };
        const uint32_t header[4] =
        {
            0x40000000u | k_iop_request, k_payload_bytes, 0u, 0u
        };
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            m_ram.write32(k_ee_request + i * 4u, header[i]);
            m_ram.write32(k_ee_request + 16u + i * 4u, payload[i]);
        }

        return ee_write(m_dmac, ee::c_ee_dmac::k_sif1_base,
                        0x10u, k_ee_request) &&
               ee_write(m_dmac, ee::c_ee_dmac::k_sif1_base,
                        0x20u, 2u) &&
               ee_write(m_dmac, ee::c_ee_dmac::k_sif1_base,
                        0u, 0x101u);
    }
};

int run_quantum(uint32_t quantum)
{
    s_fixture f{};
    if (!f.start())
    {
        return fail("fixture setup", quantum, 0u, 0u);
    }

    const iop::s_kernel_run_result initial =
        f.m_iop.run(64u, quantum);
    uint32_t dma10_chcr = 0u;
    if (initial.m_stop != iop::e_kernel_stop::budget_exhausted ||
        !f.m_iop.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u,
            4u, &dma10_chcr) ||
        (dma10_chcr & 0x01000000u) == 0u ||
        f.m_sif0->delivered_bytes() != 0u ||
        f.m_sif1->delivered_bytes() != 0u ||
        f.m_dmac.status() != 0u)
    {
        return fail("IOP DMA10 completed without an EE sender",
                    quantum, 0u, f.m_iop.cpu().state().m_pc);
    }

    for (uint32_t sequence = 1u; sequence <= 3u; ++sequence)
    {
        const uint32_t p1 = 0xfedcba98u ^ (quantum * 13u + sequence);
        const uint32_t p2 = 0x13579bdfu + sequence * 0x101u;
        const uint32_t p3 = sequence ^ p1 ^ p2;
        const uint32_t expected[4] =
        {
            sequence, p1 ^ 0x5aa5u, p2 + 7u, ~p3
        };

        // Ack the previous transfer independently on EE and IOP.
        // IOP I_STAT uses write-zero-to-ack; DICR2 and D_STAT use
        // write-one-to-clear on their channel pending bits.
        const ee::s_ee_u128 clear_ee{(1u << 5u) | (1u << 6u), 0u};
        if (!ee::c_ee_dmac::mmio_write(
                &f.m_dmac, ee::c_ee_dmac::k_stat_address,
                4u, &clear_ee) ||
            !f.m_iop.hardware().write(iop::c_hw::k_i_stat, 4u,
                                      ~(1u << iop::c_hw::k_dma_irq)) ||
            !f.m_iop.hardware().write(
                iop::c_hw::k_dma_icr2, 4u,
                (1u << 26u) | (1u << 27u)))
        {
            return fail("could not acknowledge prior DMA statuses",
                        quantum, sequence, f.m_iop.cpu().state().m_pc);
        }

        // EE destination is guarded before every response. No stale
        // packet is allowed to satisfy a later sequence.
        for (uint32_t address = k_ee_reply - 16u;
             address < k_ee_reply + k_payload_bytes + 16u;
             address += 4u)
        {
            f.m_ram.write32(address, 0xcdcdcdcdu);
        }
        if (!f.arm_ee_receiver() ||
            !f.send_request(sequence, p1, p2, true))
        {
            return fail("failed to arm EE DMA5/DMA6",
                        quantum, sequence, f.m_iop.cpu().state().m_pc);
        }

        const uint64_t expected_bytes =
            static_cast<uint64_t>(sequence) * k_payload_bytes;
        bool completed = false;
        constexpr uint32_t k_max_rounds = 128u;
        for (uint32_t attempt = 0u; attempt < k_max_rounds; ++attempt)
        {
            if (f.m_sif0->delivered_bytes() == expected_bytes)
            {
                completed = true;
                break;
            }
            const iop::s_kernel_run_result run =
                f.m_iop.run(quantum, quantum);
            if (run.m_stop != iop::e_kernel_stop::budget_exhausted)
            {
                return fail("guest stopped before valid reply",
                            quantum, sequence, run.m_guest_pc);
            }
        }
        if (!completed && f.m_sif0->delivered_bytes() == expected_bytes)
        {
            completed = true;
        }

        uint32_t ee_sif0_chcr = 0u;
        uint32_t ee_sif1_chcr = 0u;
        uint32_t iop_sif0_chcr = 0u;
        uint32_t iop_sif1_chcr = 0u;
        uint32_t dicr2 = 0u;
        uint32_t istat = 0u;
        uint32_t source_checksum = 0u;
        uint32_t destination_checksum = 0u;

        if (!completed || f.m_sif0->faulted() || f.m_sif1->faulted() ||
            f.m_sif1->delivered_bytes() != expected_bytes ||
            f.m_sif0->delivered_bytes() != expected_bytes ||
            !ee_read(f.m_dmac, ee::c_ee_dmac::k_sif0_base,
                     0u, &ee_sif0_chcr) ||
            !ee_read(f.m_dmac, ee::c_ee_dmac::k_sif1_base,
                     0u, &ee_sif1_chcr) ||
            (ee_sif0_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
            (ee_sif1_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
            (f.m_dmac.status() & 0x60u) != 0x60u ||
            !f.m_iop.hardware().read(
                iop::c_hw::k_sif0_dma_base + 8u,
                4u, &iop_sif0_chcr) ||
            !f.m_iop.hardware().read(
                iop::c_hw::k_sif1_dma_base + 8u,
                4u, &iop_sif1_chcr) ||
            (iop_sif0_chcr & 0x01000000u) != 0u ||
            (iop_sif1_chcr & 0x01000000u) != 0u ||
            !f.m_iop.hardware().read(iop::c_hw::k_dma_icr2,
                                     4u, &dicr2) ||
            (dicr2 & 0x0c000000u) != 0x0c000000u ||
            !f.m_iop.hardware().read(iop::c_hw::k_i_stat, 4u, &istat) ||
            (istat & (1u << iop::c_hw::k_dma_irq)) == 0u)
        {
            std::fprintf(stderr,
                "roundtrip state: completed=%u sif0fault=%u sif1fault=%u "
                "delivered0=%llu delivered1=%llu DSTAT=%08x "
                "EEchcr0=%08x EEchcr1=%08x IOPchcr9=%08x IOPchcr10=%08x "
                "DICR2=%08x ISTAT=%08x\n",
                completed ? 1u : 0u,
                f.m_sif0->faulted() ? 1u : 0u,
                f.m_sif1->faulted() ? 1u : 0u,
                static_cast<unsigned long long>(f.m_sif0->delivered_bytes()),
                static_cast<unsigned long long>(f.m_sif1->delivered_bytes()),
                f.m_dmac.status(), ee_sif0_chcr, ee_sif1_chcr,
                iop_sif0_chcr, iop_sif1_chcr, dicr2, istat);
            return fail("DMA completion/status/interrupt mismatch",
                        quantum, sequence, f.m_iop.cpu().state().m_pc);
        }

        const uint32_t request[4] = {sequence, p1, p2, p3};
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            uint32_t iop_received = 0u;
            uint32_t iop_transformed = 0u;
            if (!f.m_iop.cpu().read32(
                    k_iop_request + i * 4u, &iop_received) ||
                !f.m_iop.cpu().read32(
                    k_iop_reply + i * 4u, &iop_transformed) ||
                iop_received != request[i] ||
                iop_transformed != expected[i] ||
                f.m_ram.read32(k_ee_reply + i * 4u) != expected[i] ||
                f.m_ram.read32(k_ee_request + 16u + i * 4u) != request[i])
            {
                return fail("guest validation, transform or exact bytes",
                            quantum, sequence, f.m_iop.cpu().state().m_pc);
            }
            source_checksum += checksum(iop_transformed);
            destination_checksum +=
                checksum(f.m_ram.read32(k_ee_reply + i * 4u));
        }

        for (uint32_t offset = 0u; offset < 16u; offset += 4u)
        {
            if (f.m_ram.read32(k_ee_reply - 16u + offset) !=
                    0xcdcdcdcdu ||
                f.m_ram.read32(
                    k_ee_reply + k_payload_bytes + offset) !=
                    0xcdcdcdcdu)
            {
                return fail("EE reply guard overwritten",
                            quantum, sequence, f.m_iop.cpu().state().m_pc);
            }
        }

        std::printf(
            "{\"direction\":\"roundtrip\",\"quantum\":%u,"
            "\"sequence\":%u,\"requested_bytes\":16,"
            "\"transferred_bytes\":16,\"source\":%u,"
            "\"destination\":%u,\"source_checksum\":%u,"
            "\"destination_checksum\":%u,\"ee_dstat\":%u,"
            "\"iop_dicr2\":%u,\"iop_istat\":%u,"
            "\"completion\":\"guest_dma9_dma10\"}\n",
            quantum, sequence, k_iop_reply, k_ee_reply,
            source_checksum, destination_checksum,
            f.m_dmac.status(), dicr2, istat);
    }

    // A fourth packet with a bad checksum reaches the guest via DMA10,
    // but the ROM's own branch to BREAK must reject it. No DMA9 reply
    // and no EE SIF0 completion may be generated.
    const uint64_t delivered_before = f.m_sif0->delivered_bytes();
    for (uint32_t at = k_ee_reply - 16u;
         at < k_ee_reply + 32u; at += 4u)
    {
        f.m_ram.write32(at, 0xcdcdcdcdu);
    }
    const ee::s_ee_u128 clear_ee{1u << 5u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &f.m_dmac, ee::c_ee_dmac::k_stat_address, 4u,
            &clear_ee) ||
        !f.arm_ee_receiver() ||
        !f.send_request(4u, 0x12345678u, 0xaabbccddu, false))
    {
        return fail("invalid payload fixture setup",
                    quantum, 4u, f.m_iop.cpu().state().m_pc);
    }
    bool guest_rejected = false;
    for (uint32_t attempt = 0u; attempt < 128u; ++attempt)
    {
        const iop::s_kernel_run_result run =
            f.m_iop.run(quantum, quantum);
        if (run.m_stop == iop::e_kernel_stop::breakpoint)
        {
            guest_rejected = true;
            break;
        }
        if (run.m_stop != iop::e_kernel_stop::budget_exhausted)
        {
            return fail("guest failed before checksum rejection",
                        quantum, 4u, run.m_guest_pc);
        }
    }
    uint32_t ee_chcr = 0u;
    if (!guest_rejected ||
        f.m_sif0->delivered_bytes() != delivered_before ||
        !ee_read(f.m_dmac, ee::c_ee_dmac::k_sif0_base,
                 0u, &ee_chcr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) == 0u ||
        (f.m_dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("invalid guest packet fabricated SIF0 reply",
                    quantum, 4u, f.m_iop.cpu().state().m_pc);
    }
    for (uint32_t at = k_ee_reply - 16u;
         at < k_ee_reply + 32u; at += 4u)
    {
        if (f.m_ram.read32(at) != 0xcdcdcdcdu)
        {
            return fail("invalid guest packet corrupted EE RAM",
                        quantum, 4u, f.m_iop.cpu().state().m_pc);
        }
    }
    return 0;
}
} // namespace

int main()
{
    const uint32_t quanta[] = {1u, 16u, 256u, 4096u};
    for (uint32_t quantum : quanta)
    {
        if (run_quantum(quantum) != 0)
        {
            return 1;
        }
    }
    std::printf("SIF DMA guest bidirectional roundtrip: PASS\n");
    return 0;
}
