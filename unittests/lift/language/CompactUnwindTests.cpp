//===- CompactUnwindTests.cpp - x86 compact unwind tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "CompactUnwindTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::macho_unwind;
using namespace neverd::compact_unwind_test;

TEST(CompactUnwindX86_64Frame, DecodesSavedRegisterSlots) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(
      Arch::X64, x86_64FrameEncoding(3, {CU_RBX, CU_R14, CU_R15}), Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{RBX, R14, R15}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({RBX, R14, R15}));
  EXPECT_EQ(Entry.SavedFPRMask, 0u);
  // Three saved registers put slot zero three pointers below the frame
  // pointer, which is where the deepest of them sits.
  EXPECT_EQ(Entry.FrameOffset, 24u);
  // A frame form establishes no frame size of its own.
  EXPECT_FALSE(Entry.HasStackSize);
}

TEST(CompactUnwindX86_64Frame, KeepsInteriorGapButNotTrailingOnes) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(
      Arch::X64, x86_64FrameEncoding(3, {CU_RBX, 0, CU_R12}), Entry));

  // The gap has to survive: r12 sits two pointers above rbx, not one, and
  // dropping the empty slot would move it.
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{RBX, -1, R12}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({RBX, R12}));
}

TEST(CompactUnwindX86_64Frame, EmptyEncodingSavesNothing) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::X64, x86_64FrameEncoding(0, {}), Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
  EXPECT_EQ(Entry.FrameOffset, 0u);
}

TEST(CompactUnwindX86_64Frame, RejectsFramePointerInASavedSlot) {
  CompactUnwindEntry Entry;
  // The frame form has already spent rbp on the frame, so a slot claiming to
  // hold it describes a frame no unwinder can walk.
  EXPECT_FALSE(decodeEncoding(Arch::X64,
                              x86_64FrameEncoding(2, {CU_RBX, CU_RBP}), Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
}

TEST(CompactUnwindX86_64Frame, RejectsSlotNumberWithNoRegister) {
  CompactUnwindEntry Entry;
  EXPECT_FALSE(decodeEncoding(Arch::X64, x86_64FrameEncoding(1, {7}), Entry));
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
}

//===----------------------------------------------------------------------===//
// x86-64 frameless permutation
//===----------------------------------------------------------------------===//

class CompactUnwindPermutation
    : public ::testing::TestWithParam<std::vector<uint32_t>> {};

TEST_P(CompactUnwindPermutation, RoundTripsThroughTheReferenceEncoder) {
  const std::vector<uint32_t> &Regs = GetParam();
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(
      Arch::X64, x86_64FramelessEncoding(kX86_64ModeStackImmediate, 8, 0, Regs),
      Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramelessImmediate);
  EXPECT_EQ(slotRegisters(Entry), machineNumbers(Regs));
  EXPECT_EQ(Entry.StackSize, 64u);
  EXPECT_TRUE(Entry.HasStackSize);
}

INSTANTIATE_TEST_SUITE_P(
    SavedRegisterOrders, CompactUnwindPermutation,
    ::testing::Values(
        std::vector<uint32_t>{CU_RBX}, std::vector<uint32_t>{CU_RBP},
        std::vector<uint32_t>{CU_RBP, CU_R12},
        std::vector<uint32_t>{CU_R12, CU_RBP},
        std::vector<uint32_t>{CU_RBX, CU_R12, CU_R13},
        std::vector<uint32_t>{CU_R13, CU_RBX, CU_R12},
        // The worked example in LLVM's own encoder comment.
        std::vector<uint32_t>{CU_RBP, CU_R12, CU_R14, CU_R15},
        std::vector<uint32_t>{CU_R15, CU_R14, CU_R13, CU_R12},
        std::vector<uint32_t>{CU_RBX, CU_R12, CU_R13, CU_R14, CU_R15},
        std::vector<uint32_t>{CU_R15, CU_R14, CU_R13, CU_R12, CU_RBX},
        // Six registers encode only five digits: the last one is
        // whatever the others did not claim.
        std::vector<uint32_t>{CU_RBX, CU_R12, CU_R13, CU_R14, CU_R15, CU_RBP},
        std::vector<uint32_t>{CU_RBP, CU_R15, CU_R14, CU_R13, CU_R12, CU_RBX}));

TEST(CompactUnwindX86_64Frameless, ExhaustivelyRoundTripsEveryOrder) {
  // Every ordered subset of the six registers: the prefixes of every
  // permutation of all six enumerate exactly those.  A divisor row that is
  // wrong for one register count, or a re-expansion that mis-ranks one digit,
  // cannot survive this.
  std::vector<uint32_t> All = {1, 2, 3, 4, 5, 6};
  do {
    for (unsigned Count = 1; Count <= All.size(); ++Count) {
      const std::vector<uint32_t> Regs(All.begin(), All.begin() + Count);
      CompactUnwindEntry Entry;
      ASSERT_TRUE(decodeEncoding(
          Arch::X64,
          x86_64FramelessEncoding(kX86_64ModeStackImmediate, 0, 0, Regs),
          Entry));
      ASSERT_EQ(slotRegisters(Entry), machineNumbers(Regs));
    }
  } while (std::next_permutation(All.begin(), All.end()));
}

TEST(CompactUnwindX86_64Frameless, RejectsRegisterCountWithNoEncoding) {
  CompactUnwindEntry Entry;
  // Seven fits the three-bit count field but names more registers than the
  // permutation alphabet has.
  EXPECT_FALSE(
      decodeEncoding(Arch::X64, kX86_64ModeStackImmediate | (7u << 10), Entry));
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
}

TEST(CompactUnwindX86_64Frameless, RejectsPermutationOutOfRange) {
  CompactUnwindEntry Entry;
  // One register can only be one of six, so a digit of six has no register to
  // land on and every register after it would be renamed by a guess.
  EXPECT_FALSE(decodeEncoding(
      Arch::X64, kX86_64ModeStackImmediate | (1u << 10) | 6u, Entry));
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
}

//===----------------------------------------------------------------------===//
// i386
//===----------------------------------------------------------------------===//

TEST(CompactUnwindX86, UsesItsOwnRegisterTable) {
  CompactUnwindEntry Entry;
  // Slot numbers two and three name ecx and edx here, where the 64-bit table
  // has r12 and r13 at the same numbers.
  ASSERT_TRUE(decodeEncoding(
      Arch::X86, kX86ModeEBPFrame | (2u << 16) | frameRegisters({1, 2}),
      Entry));

  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{3 /* ebx */, 1 /* ecx */}));
  // An i386 slot is four bytes, not eight.
  EXPECT_EQ(Entry.FrameOffset, 8u);
}

TEST(CompactUnwindX86, ScalesTheImmediateStackSizeByFour) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(
      decodeEncoding(Arch::X86, kX86ModeStackImmediate | (10u << 16), Entry));
  EXPECT_EQ(Entry.StackSize, 40u);
  EXPECT_TRUE(Entry.HasStackSize);
}

} // namespace
