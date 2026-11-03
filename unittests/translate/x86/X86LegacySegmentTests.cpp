//===- X86LegacySegmentTests.cpp - Protected operand boundaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cached segment permissions and complete operands before paged RAM effects.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/X86/X86LegacySegments.h"
#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

TEST(X86LegacySegments, WholeOffsetExtentPrecedesWrappingBaseAddition) {
  X86LegacySegmentCache C{0xffff0000, 0x4ffff, 0, 0x4093, false};
  for (auto Segment :
       {X86SegmentRegister::DS, X86SegmentRegister::SS, X86SegmentRegister::ES,
        X86SegmentRegister::FS, X86SegmentRegister::GS}) {
    for (unsigned Bytes : {1u, 2u, 4u, 16u, 64u}) {
      const auto Check = [&](uint32_t Offset) {
        return llvm::cantFail(checkX86LegacySegmentAccess(
            C, {Segment, X86SegmentAccessKind::Read, Offset, Bytes, {}}));
      };
      auto R = Check(0x30000);
      EXPECT_FALSE(R.FaultVector);
      EXPECT_EQ(R.Linear, 0x20000u);
      EXPECT_FALSE(Check(C.Limit - Bytes + 1).FaultVector);
      EXPECT_EQ(Check(C.Limit - Bytes + 2).FaultVector,
                Segment == X86SegmentRegister::SS ? 12 : 13);
      EXPECT_TRUE(Check(UINT32_MAX).FaultVector);
    }
  }
}

TEST(X86LegacySegments, ExpandDownUsesDBAndChecksEveryByte) {
  for (bool Big : {false, true}) {
    const uint32_t Upper = Big ? UINT32_MAX : UINT16_MAX;
    X86LegacySegmentCache C{0, 0x7fff, 16, uint16_t(Big ? 0x4097 : 0x97),
                            false};
    for (auto Segment : {X86SegmentRegister::DS, X86SegmentRegister::SS})
      for (unsigned Bytes : {1u, 2u, 4u, 16u})
        for (uint32_t Offset :
             {0x7fffu, 0x8000u, Upper - Bytes + 1, Upper - Bytes + 2, Upper}) {
          auto R = llvm::cantFail(checkX86LegacySegmentAccess(
              C, {Segment, X86SegmentAccessKind::Write, Offset, Bytes, {}}));
          const bool Fits =
              Offset > C.Limit && uint64_t(Offset) + Bytes - 1 <= Upper;
          EXPECT_EQ(!R.FaultVector, Fits);
        }
    C.Limit = Upper;
    if (Big)
      C.Attributes |= 0x8000;
    EXPECT_TRUE(llvm::cantFail(
                    checkX86LegacySegmentAccess(C, {X86SegmentRegister::SS,
                                                    X86SegmentAccessKind::Read,
                                                    Upper,
                                                    1,
                                                    {}}))
                    .FaultVector);
  }
}

TEST(X86LegacySegments, LoadedTypesAndUnusableAreNotSelectorReloads) {
  for (unsigned Type : {0u, 1u, 2u, 3u, 8u, 9u, 10u, 11u, 14u, 15u}) {
    const bool Code = Type & 8, RW = Type & 2;
    const auto Segment = Code ? X86SegmentRegister::CS : X86SegmentRegister::DS;
    X86LegacySegmentCache C{0, 0xffff, 0, uint16_t(0x90 | Type), false};
    for (auto Kind :
         {X86SegmentAccessKind::Read, X86SegmentAccessKind::Write}) {
      auto R = llvm::cantFail(
          checkX86LegacySegmentAccess(C, {Segment, Kind, 42, 4, {}}));
      const bool Allowed =
          Kind == X86SegmentAccessKind::Write ? !Code && RW : !Code || RW;
      EXPECT_EQ(!R.FaultVector, Allowed);
    }
    if (Code)
      EXPECT_FALSE(
          llvm::cantFail(
              checkX86LegacySegmentAccess(
                  C, {Segment, X86SegmentAccessKind::Execute, 42, 4, {}}))
              .FaultVector);
  }
  X86LegacySegmentCache C;
  auto R = llvm::cantFail(checkX86LegacySegmentAccess(
      C, {X86SegmentRegister::DS, X86SegmentAccessKind::Read, 0, 1, {}}));
  EXPECT_EQ(R.FaultVector, 13);
  auto Invalid = checkX86LegacySegmentAccess(
      C, {X86SegmentRegister::SS, X86SegmentAccessKind::Read, 0, 1, {}});
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
}

TEST(X86LegacySegments, FullFourGiBLimitRequiresAPerExecutionChoice) {
  X86LegacySegmentCache C{0, UINT32_MAX, 16, 0xc093, false};
  X86LegacySegmentAccess A{
      X86SegmentRegister::SS, X86SegmentAccessKind::Read, UINT32_MAX, 2, {}};
  auto Missing = checkX86LegacySegmentAccess(C, A);
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  A.FullLimitChoice = X86FullSegmentLimitChoice::Fault;
  EXPECT_EQ(llvm::cantFail(checkX86LegacySegmentAccess(C, A)).FaultVector, 12);
  A.FullLimitChoice = X86FullSegmentLimitChoice::Wrap;
  auto R = llvm::cantFail(checkX86LegacySegmentAccess(C, A));
  EXPECT_FALSE(R.FaultVector);
  EXPECT_EQ(R.Linear, UINT32_MAX);
  C.Limit = 0xffffefff;
  EXPECT_EQ(llvm::cantFail(checkX86LegacySegmentAccess(C, A)).FaultVector, 12);
}

TEST(X86LegacySegments, MalformedSnapshotsAreBindingErrors) {
  for (uint16_t Attributes : {0x4090, 0x4083, 0x4193, 0x6093, 0x4099, 0xc093}) {
    X86LegacySegmentCache C{0, 0x1234, 16, Attributes, false};
    auto E = validateX86LegacySegmentCache(X86SegmentRegister::SS, C);
    EXPECT_TRUE(bool(E)) << Attributes;
    llvm::consumeError(std::move(E));
  }
}

TEST(X86LegacySegments, PagedWrapKeepsFaultPriorityAndWholeOperandPublication) {
  X86PagingFeatures F{.PhysicalAddressBits = 32};
  X86PagingState S{.CR0 = 0x80010001, .CR3 = 0x1000};
  auto State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x9000)});
  auto M = llvm::cantFail(X86PagedMemory::create(State, F, S));
  const auto Put = [&](uint64_t Address, uint32_t Value) {
    EXPECT_EQ(M->physicalMemory()
                  .storeScalar(Address, GuestScalarWidth::I32, Value)
                  .Status,
              GuestMemoryAccessStatus::Completed);
  };
  Put(0x1ffc, 0x2007);
  Put(0x2ffc, 0x7007);
  Put(0x1000, 0x3007);
  Put(0x3000, 0); // Last linear page exists, page zero faults.
  Put(0x7ffc, 0x44332211);
  X86SegmentedAccess A{X86SegmentRegister::SS,
                       {0, UINT32_MAX, 16, 0xc093, false},
                       UINT32_MAX - 1,
                       MemoryAccessKind::Write,
                       3,
                       X86FullSegmentLimitChoice::Fault};
  const std::array<uint8_t, 4> Input{0xaa, 0xbb, 0xcc, 0xdd};
  auto R = llvm::cantFail(M->storeSegment(A, Input));
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 12);
  EXPECT_EQ(M->physicalMemory().loadScalar(0x1ffc, GuestScalarWidth::I32).Value,
            0x2007u);
  A.FullLimitChoice = X86FullSegmentLimitChoice::Wrap;
  R = llvm::cantFail(M->storeSegment(A, Input));
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 14);
  EXPECT_EQ(R.Exception->ErrorCode, 6u);
  EXPECT_EQ(R.Exception->CR2, 0u);
  EXPECT_NE(
      M->physicalMemory().loadScalar(0x1ffc, GuestScalarWidth::I32).Value &
          0x20,
      0u);
  EXPECT_EQ(M->physicalMemory().loadScalar(0x7ffc, GuestScalarWidth::I32).Value,
            0x44332211u);
  Put(0x3000, 0x8007);
  R = llvm::cantFail(M->storeSegment(A, Input));
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(M->physicalMemory().loadScalar(0x7ffc, GuestScalarWidth::I32).Value,
            0xbbaa2211u);
  EXPECT_EQ(M->physicalMemory().loadScalar(0x8000, GuestScalarWidth::I32).Value,
            0xddccu);
  A.Kind = MemoryAccessKind::Read;
  std::array<uint8_t, 4> Output{};
  R = llvm::cantFail(M->loadSegment(A, Output));
  EXPECT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(Output, Input);
  Put(0x3000, 0);
  Output.fill(0x99);
  R = llvm::cantFail(M->loadSegment(A, Output));
  EXPECT_EQ(R.Status, X86PageWalkStatus::Exception);
  EXPECT_EQ(Output, (std::array<uint8_t, 4>{0x99, 0x99, 0x99, 0x99}));
  Put(0x3000, 0x9007); // Present page, absent physical backing is not #PF.
  R = llvm::cantFail(M->loadSegment(A, Output));
  EXPECT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_FALSE(R.Exception);
  EXPECT_EQ(Output, (std::array<uint8_t, 4>{0x99, 0x99, 0x99, 0x99}));
  A.Kind = MemoryAccessKind::Write;
  R = llvm::cantFail(M->storeSegment(A, std::array<uint8_t, 4>{1, 2, 3, 4}));
  EXPECT_EQ(R.Status, X86PageWalkStatus::BackingFault);
  EXPECT_EQ(M->physicalMemory().loadScalar(0x7ffc, GuestScalarWidth::I32).Value,
            0xbbaa2211u);
  A.Kind = MemoryAccessKind::Read;
  S.RFlags = 0x20002;
  llvm::cantFail(M->bindControls(S));
  auto Invalid = M->loadSegment(A, Output);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
}

} // namespace
