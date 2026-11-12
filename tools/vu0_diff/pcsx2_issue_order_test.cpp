#include "Common.h"
#include "VUmicro.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace
{

constexpr u32 VU0_LOWER_IBEQ = 0x28u;
constexpr u32 VU0_LOWER_IADDIU = 0x08u;
constexpr u32 VU0_UPPER_NOP = 0x3fu | (11u << 6);

void store_le32(u8* destination, u32 value)
{
    destination[0] = static_cast<u8>(value);
    destination[1] = static_cast<u8>(value >> 8);
    destination[2] = static_cast<u8>(value >> 16);
    destination[3] = static_cast<u8>(value >> 24);
}

u32 make_branch(u32 primary, u32 it, u32 is, u32 immediate)
{
    return (primary << 25) | (it << 16) | (is << 11) |
           (immediate & 0x07ffu);
}

u32 make_iaddiu(u32 it, u32 is, u32 immediate)
{
    return (VU0_LOWER_IADDIU << 25) | (it << 16) | (is << 11) |
           ((immediate & 0x7800u) << 10) | (immediate & 0x07ffu);
}

class VU0IssueOrder : public ::testing::Test
{
protected:
    std::array<u8, VU0_PROGSIZE> m_micro{};
    std::array<u8, VU0_MEMSIZE> m_data{};

    void SetUp() override
    {
        VU0 = VURegs{};
        VU0.Micro = m_micro.data();
        VU0.Mem = m_data.data();
        VU0.VF[0].f.x = 0.0f;
        VU0.VF[0].f.y = 0.0f;
        VU0.VF[0].f.z = 0.0f;
        VU0.VF[0].f.w = 1.0f;
        VU0.VI[0].UL = 0;
        VU0.VI[REG_TPC].UL = 0;
        VU0.VI[REG_VPU_STAT].UL = 1;
    }

    void TearDown() override
    {
        VU0.Micro = nullptr;
        VU0.Mem = nullptr;
    }

    void install_pair(u32 byte_pc, u32 lower, u32 upper = VU0_UPPER_NOP)
    {
        store_le32(&m_micro[byte_pc], lower);
        store_le32(&m_micro[byte_pc + 4u], upper);
    }

    void seed_ialu_pipe(u32 index, u32 register_mask, u64 start_cycle, u32 latency)
    {
        VU0.ialu[index].reg = static_cast<int>(register_mask);
        VU0.ialu[index].sCycle = start_cycle;
        VU0.ialu[index].Cycle = latency;
    }

    void execute_one_pair()
    {
        vu0Exec(&VU0);
    }
};

} // namespace

TEST_F(VU0IssueOrder, BranchInterlockExpiresBackupBeforePredicateRead)
{
    VU0.cycle = 10;
    VU0.VI[1].UL = 0;
    VU0.VI[0].UL = 0;
    VU0.VIBackupCycles = 1;
    VU0.VIRegNumber = 1;
    VU0.VIOldValue = 1;

    seed_ialu_pipe(0, 1u << 1, 10, 3);
    VU0.ialureadpos = 0;
    VU0.ialuwritepos = 1;
    VU0.ialucount = 1;

    install_pair(0, make_branch(VU0_LOWER_IBEQ, 1, 0, 2));

    execute_one_pair();

    EXPECT_EQ(VU0.cycle, 13u);
    EXPECT_EQ(VU0.VI[REG_TPC].UL, 8u);
    EXPECT_EQ(VU0.branch, 1u);
    EXPECT_EQ(VU0.branchpc, 24u);
    EXPECT_EQ(VU0.VIBackupCycles, 0u);
    EXPECT_EQ(VU0.ialucount, 0u);
}

TEST_F(VU0IssueOrder, TwoSourceInterlockWaitsForLatestCompletion)
{
    VU0.cycle = 20;
    VU0.VI[1].UL = 0;
    VU0.VI[2].UL = 1;
    VU0.VIBackupCycles = 1;
    VU0.VIRegNumber = 1;
    VU0.VIOldValue = 1;

    seed_ialu_pipe(0, 1u << 1, 20, 3);
    seed_ialu_pipe(1, 1u << 2, 20, 5);
    VU0.ialureadpos = 0;
    VU0.ialuwritepos = 2;
    VU0.ialucount = 2;

    install_pair(0, make_branch(VU0_LOWER_IBEQ, 1, 2, 2));

    execute_one_pair();

    EXPECT_EQ(VU0.cycle, 25u);
    EXPECT_EQ(VU0.VI[REG_TPC].UL, 8u);
    EXPECT_EQ(VU0.branch, 0u);
    EXPECT_EQ(VU0.VIBackupCycles, 0u);
    EXPECT_EQ(VU0.ialucount, 0u);
}

TEST_F(VU0IssueOrder, IALUWriteStartsFreshTwoCycleBackup)
{
    VU0.cycle = 30;
    VU0.VI[1].UL = 0xabcd0007u;

    install_pair(0, make_iaddiu(1, 0, 0x1234u));

    execute_one_pair();

    EXPECT_EQ(VU0.cycle, 31u);
    EXPECT_EQ(VU0.VI[1].US[0], 0x1234u);
    EXPECT_EQ(VU0.VIBackupCycles, 2u);
    EXPECT_EQ(VU0.VIRegNumber, 1u);
    EXPECT_EQ(VU0.VIOldValue, 7u);
}
