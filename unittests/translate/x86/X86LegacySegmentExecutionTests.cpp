//===- X86LegacySegmentExecutionTests.cpp - Selector transition contracts -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Refusal, transactional publication, cached state and paged fault ordering.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/X86LegacySegmentExecution.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

class X86LegacySegmentExecutionTest : public testing::Test {
protected:
  X86LegacySegmentState S;
  X86LegacySegmentExecutionOptions O{
      X86LegacySegmentProfile::WordStackOffsetFirst,
      X86AlignmentPriority::AfterPaging,
      {}};
  std::unique_ptr<X86PagedMemory> M;

  void SetUp() override {
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x10000)});
    M = llvm::cantFail(X86PagedMemory::create(
        RAM, {.PhysicalAddressBits = 32},
        {.CR0 = 0x80050001, .CR3 = 0x1000, .RFlags = 0x40002}));
    put(0x1000, 0x2007, GuestScalarWidth::I32);
    for (unsigned I = 0; I != 16; ++I)
      put(0x2000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
    S.CPL = 3;
    for (auto &C : S.Segments)
      C = {0, UINT32_MAX, 35, 0xc0f3, false};
    S.Segments[1] = {0, UINT32_MAX, 27, 0xc0fb, false};
    S.GDTRBase = 0x5000;
    S.GDTRLimit = 63;
    put(0x5038, 0x00cff2000000ffffULL);
    S.GPR[0] = 59;
    S.GPR[4] = 0x8000;
    S.GPR[7] = 0x9000;
    put(0x8000, 59);
    put(0x9000, 59);
  }

  void put(uint64_t Address, uint64_t Value,
           GuestScalarWidth Width = GuestScalarWidth::I64) {
    ASSERT_EQ(M->physicalMemory().storeScalar(Address, Width, Value).Status,
              GuestMemoryAccessStatus::Completed);
  }

  uint64_t get(uint64_t Address,
               GuestScalarWidth Width = GuestScalarWidth::I64) {
    auto R = M->physicalMemory().loadScalar(Address, Width);
    EXPECT_EQ(R.Status, GuestMemoryAccessStatus::Completed);
    return R.Value;
  }

  X86LegacySegmentExecutionResult run(llvm::ArrayRef<uint8_t> Bytes) {
    return llvm::cantFail(executeX86LegacySegmentInstruction(S, *M, Bytes, O));
  }
};

TEST_F(X86LegacySegmentExecutionTest,
       NullSelectorsNeverReadTheDescriptorTable) {
  S.GDTRBase = 0xf000;
  put(0x203c, 0, GuestScalarWidth::I32);
  for (unsigned Segment : {0u, 2u, 3u, 4u, 5u}) {
    const auto Old = S.Segments[Segment];
    S.GPR[0] = 3;
    const std::array<uint8_t, 2> Bytes{0x8e, uint8_t(0xc0 | Segment * 8)};
    auto R = run(Bytes);
    if (Segment == 2) {
      ASSERT_TRUE(R.Exception);
      EXPECT_EQ(R.Exception->Vector, 13);
      EXPECT_EQ(R.Exception->ErrorCode, 0u);
      EXPECT_EQ(S.Segments[Segment].Selector, Old.Selector);
    } else {
      EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
      EXPECT_EQ(S.Segments[Segment].Selector, 3);
      EXPECT_TRUE(S.Segments[Segment].Unusable);
      EXPECT_EQ(S.Segments[Segment].Base, Old.Base);
    }
  }
  EXPECT_EQ(get(0x1000, GuestScalarWidth::I32), 0x2007u);
}

TEST_F(X86LegacySegmentExecutionTest,
       AccessedWriteFaultPreservesCacheAndStack) {
  put(0x2014, 0x5005, GuestScalarWidth::I32);
  auto R =
      run({0x17}); // POP SS: selector read succeeds, descriptor write fails.
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 14);
  EXPECT_EQ(R.Exception->ErrorCode, 3u); // Implicit supervisor write at CPL3.
  EXPECT_EQ(R.Exception->CR2, 0x5038u);
  EXPECT_EQ(S.GPR[4], 0x8000u);
  EXPECT_EQ(S.Segments[2].Selector, 35);
  EXPECT_FALSE(R.LoadsSSWithInhibition);
  EXPECT_EQ(get(0x5038), 0x00cff2000000ffffULL);
  put(0x5038, 0x00cff3000000ffffULL);
  R = run({0x17});
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(S.GPR[4], 0x8004u);
  EXPECT_EQ(S.Segments[2].Selector, 59);
  EXPECT_TRUE(R.LoadsSSWithInhibition);
}

TEST_F(X86LegacySegmentExecutionTest, FarPointerStagesBothDestinations) {
  S.GPR[7] = 0x9ffc;
  put(0x9ffc, 0xdeadbeef, GuestScalarWidth::I32);
  put(0xa000, 59);
  put(0x2028, 0, GuestScalarWidth::I32);
  auto R = run({0x0f, 0xb4, 0x3f}); // LFS EDI,[EDI].
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->CR2, 0xa000u);
  EXPECT_EQ(S.GPR[7], 0x9ffcu);
  EXPECT_EQ(S.Segments[4].Selector, 35);
  EXPECT_EQ(get(0x5038), 0x00cff2000000ffffULL);
  put(0x2028, 0xa007, GuestScalarWidth::I32);
  R = run({0x0f, 0xb4, 0x3f});
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(S.GPR[7], 0xdeadbeefu);
  EXPECT_EQ(S.Segments[4].Selector, 59);
  EXPECT_EQ(get(0x5038), 0x00cff3000000ffffULL);
}

TEST_F(X86LegacySegmentExecutionTest,
       StackSizeUsesOldSSAndPreservesESPHighWord) {
  S.Segments[2] = {0, 0xffff, 35, 0xf3, false};
  S.GPR[4] = 0x1234fffc;
  put(0xfffc, 59, GuestScalarWidth::I16);
  auto R = run({0x67, 0x17});
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(S.GPR[4], 0x12340000u);
  EXPECT_EQ(S.Segments[2].Attributes, 0xc0f3);
  EXPECT_TRUE(R.LoadsSSWithInhibition);
}

TEST_F(X86LegacySegmentExecutionTest,
       AlignmentAndSegmentPriorityNeverPublishAStore) {
  S.GPR[7] = 0x9001;
  const auto Old = get(0x9000);
  put(0x2024, 0, GuestScalarWidth::I32);
  for (auto Priority : {X86AlignmentPriority::BeforePaging,
                        X86AlignmentPriority::AfterPaging}) {
    O.AlignmentPriority = Priority;
    auto R = run({0x8c, 0x1f});
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector,
              Priority == X86AlignmentPriority::BeforePaging ? 17 : 14);
    EXPECT_EQ(get(0x9000), Old);
  }
  S.Segments[3].Unusable = true;
  auto R = run({0x8c, 0x1f});
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
  EXPECT_EQ(get(0x9000), Old);
}

TEST_F(X86LegacySegmentExecutionTest, AdmissionRefusesBeforeAnyPageEffect) {
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{},
                                           {0x8e, 0xc8},
                                           {0xf0, 0x8e, 0xd8},
                                           {0x66, 0x66, 0x8e, 0xd8},
                                           {0x8e},
                                           {0x0f, 0xb4, 0xc0},
                                           {0x8e, 0xd8, 0x90},
                                           {0x0f},
                                           {0x40, 0x8e, 0xd8}}) {
    auto R = executeX86LegacySegmentInstruction(S, *M, Bytes, O);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
  }
  O.Profile.reset();
  auto Unbound = executeX86LegacySegmentInstruction(S, *M, {0x8e, 0xd8}, O);
  EXPECT_FALSE(bool(Unbound));
  if (!Unbound)
    llvm::consumeError(Unbound.takeError());
  O.Profile = X86LegacySegmentProfile::WordStackOffsetFirst;
  O.AlignmentPriority.reset();
  auto R = executeX86LegacySegmentInstruction(S, *M, {0x8e, 0xd8}, O);
  EXPECT_FALSE(bool(R));
  if (!R)
    llvm::consumeError(R.takeError());
  EXPECT_EQ(get(0x1000, GuestScalarWidth::I32), 0x2007u);
  EXPECT_EQ(S.Segments[3].Selector, 35);
}

TEST_F(X86LegacySegmentExecutionTest,
       PhysicalBackingStopIsNotAnArchitecturalFault) {
  put(0x2014, 0x20007, GuestScalarWidth::I32);
  auto R = run({0x1f});
  EXPECT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_FALSE(R.Exception);
  EXPECT_TRUE(R.BackingFault);
  EXPECT_EQ(S.GPR[4], 0x8000u);
  EXPECT_EQ(S.Segments[3].Selector, 35);
}

} // namespace
