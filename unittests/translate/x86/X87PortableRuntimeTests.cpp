//===- X87PortableRuntimeTests.cpp - Portable x87 memory transactions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/X87PortableRuntime.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

constexpr uint32_t Load80 =
    0x28db | X87ContextMemory | (0x28u << X87ContextOriginalModRMShift);
constexpr uint32_t Store80 =
    0x38db | X87ContextMemory | (0x38u << X87ContextOriginalModRMShift);

std::array<uint8_t, 10> binary80(uint64_t Significand, uint16_t Exponent) {
  std::array<uint8_t, 10> Bytes{};
  for (unsigned I = 0; I < 8; ++I)
    Bytes[I] = Significand >> (I * 8);
  Bytes[8] = Exponent;
  Bytes[9] = Exponent >> 8;
  return Bytes;
}

PortableX87StateV1 environment(uint16_t CW = 0x037f, unsigned Top = 5) {
  PortableX87StateV1 State;
  State.EnvironmentKnown = 1;
  State.Image[0] = CW;
  State.Image[1] = CW >> 8;
  State.Image[3] = Top << 3;
  // Retained raw payloads do not imply known payloads, even with empty tags.
  for (unsigned I = 32; I < 160; ++I)
    State.Image[I] = uint8_t(I ^ 0xa5);
  return State;
}

uint16_t word(const PortableX87StateV1 &State, unsigned Offset) {
  return State.Image[Offset] | uint16_t(State.Image[Offset + 1]) << 8;
}

uint64_t address(const PortableX87StateV1 &State, unsigned Offset) {
  uint64_t Value = 0;
  for (unsigned I = 0; I < 8; ++I)
    Value |= uint64_t(State.Image[Offset + I]) << (I * 8);
  return Value;
}

std::unique_ptr<GuestMemoryRuntime>
memory(std::vector<GuestMemoryRegion> Regions,
       CodeInvalidationPolicy Policy =
           CodeInvalidationPolicy::InvalidateOnExecutableWrite) {
  auto State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory = std::move(Regions);
  GuestMemoryRuntimeConfig Config;
  Config.CodeInvalidation = Policy;
  return llvm::cantFail(GuestMemoryRuntime::create(State, Config));
}

void expectUnchanged(const PortableX87StateV1 &Before,
                     const PortableX87StateV1 &After) {
  EXPECT_EQ(std::memcmp(&Before, &After, sizeof(Before)), 0);
}

TEST(X87PortableRuntime, ResetsNeedNoGuestMemoryAndRetainCompletionBoundaries) {
  auto Memory = memory({});
  for (unsigned Top = 0; Top != 8; ++Top)
    for (uint32_t Selector : {0xd0d9U, 0x770fU, 0x0e0fU}) {
      auto State = environment(0x37f, Top);
      State.Image[4] = 0xff;
      State.KnownPayload = 0x96;
      const auto Before = State;
      auto Step = executePortableX87RuntimeStepV1(State, *Memory, Selector,
                                                  0x4000, UINT64_MAX);
      ASSERT_EQ(Step.Status, PortableX87StepStatusV1::Completed);
      EXPECT_TRUE(Step.InstructionCompleted);
      EXPECT_FALSE(Step.MemoryAccess);
      EXPECT_FALSE(Step.HasScalarResult);
      auto Expected = Before;
      if (Selector == 0xd0d9)
        Expected.Image[9] = 0x40;
      else {
        Expected.Image[3] = 0;
        Expected.Image[4] = 0;
        for (unsigned Physical = 0; Physical != 8; ++Physical)
          std::copy_n(Before.Image.begin() + 32 +
                          ((Physical + 8 - Top) & 7) * 16,
                      16, Expected.Image.begin() + 32 + Physical * 16);
      }
      if (Selector == 0x0e0f)
        Expected.KnownPayload = 0;
      expectUnchanged(Expected, State);
      // A failed next instruction keeps the reset's completed prefix.
      const auto Reset = State;
      Step = executePortableX87RuntimeStepV1(State, *Memory, Load80, 0x4002,
                                             UINT64_MAX);
      EXPECT_EQ(Step.Status, PortableX87StepStatusV1::MemoryFault);
      EXPECT_FALSE(Step.InstructionCompleted);
      expectUnchanged(Reset, State);
      State.Image[0] &= ~1U;
      State.Image[2] |= 0x81;
      const auto Pending = State;
      Step = executePortableX87RuntimeStepV1(State, *Memory, Selector, 0x4004,
                                             UINT64_MAX);
      EXPECT_EQ(Step.Status, PortableX87StepStatusV1::PendingException);
      EXPECT_FALSE(Step.InstructionCompleted);
      EXPECT_FALSE(Step.MemoryAccess);
      expectUnchanged(Pending, State);
    }
}

TEST(X87PortableRuntime, EveryTopAndPrecisionRoundingRetainsBinary80) {
  const auto A = binary80(0x8000000000000001, 0x3fff);
  const auto B = binary80(0x8000000000000000, 0x3ffe);
  for (unsigned Top = 0; Top < 8; ++Top)
    for (unsigned PC : {0u, 2u, 3u})
      for (unsigned RC = 0; RC < 4; ++RC) {
        SCOPED_TRACE((::testing::Message() << Top << '/' << PC << '/' << RC));
        auto State = environment(0x7f | PC << 8 | RC << 10, Top);
        auto Memory = memory({
            {0x1000, MemoryPermission::Read, 0, {A.begin(), A.end()}},
            {0x2000, MemoryPermission::Read, 0, {B.begin(), B.end()}},
            {0x3000, MemoryPermission::Write, 0,
             std::vector<uint8_t>(12, 0xa5)},
        });
        auto First = executePortableX87RuntimeStepV1(State, *Memory, Load80,
                                                     0x4000, 0x1000);
        ASSERT_EQ(First.Status, PortableX87StepStatusV1::Completed);
        EXPECT_TRUE(First.InstructionCompleted);
        EXPECT_FALSE(First.HasScalarResult);
        ASSERT_EQ(executePortableX87RuntimeStepV1(State, *Memory, Load80,
                                                  0x4002, 0x2000)
                      .Status,
                  PortableX87StepStatusV1::Completed);
        const auto Added =
            executePortableX87RuntimeStepV1(State, *Memory, 0xc1de, 0x4004, 0);
        ASSERT_EQ(Added.Status, PortableX87StepStatusV1::Completed);
        EXPECT_FALSE(Added.MemoryAccess);
        const bool Rounded = PC != 3;
        EXPECT_EQ(word(State, 2) & 0x220,
                  Rounded ? 0x20u | (RC == 2 ? 0x200u : 0u) : 0u);
        ASSERT_EQ(executePortableX87RuntimeStepV1(State, *Memory, Store80,
                                                  0x4006, 0x3001)
                      .Status,
                  PortableX87StepStatusV1::Completed);
        uint64_t Mantissa = 0xc000000000000000;
        if (PC == 3)
          Mantissa |= 1;
        else if (RC == 2)
          Mantissa |= uint64_t{1} << (PC == 2 ? 11 : 40);
        const auto Expected = binary80(Mantissa, 0x3fff);
        const auto Output = Memory->snapshotMemoryRegions()[2].Bytes;
        EXPECT_EQ(Output.front(), 0xa5);
        EXPECT_EQ(Output.back(), 0xa5);
        EXPECT_TRUE(
            std::equal(Expected.begin(), Expected.end(), Output.begin() + 1));
        EXPECT_EQ(word(State, 0), 0x7f | PC << 8 | RC << 10);
        EXPECT_EQ(word(State, 2), (Top << 11) | (Rounded ? 0x20u : 0u));
        EXPECT_EQ(State.Image[4], 0);
        EXPECT_EQ(State.KnownPayload,
                  (1u << ((Top + 7) & 7)) | (1u << ((Top + 6) & 7)));
        EXPECT_EQ(address(State, 8), 0x4006u);
        EXPECT_EQ(address(State, 16), 0x3001u);
      }
}

TEST(X87PortableRuntime, FaultingStoreKeepsTheCompletedPrefixAndCurrentStack) {
  const auto A = binary80(0x8000000000000001, 0x3fff);
  auto State = environment();
  auto Memory = memory({
      {0x1000, MemoryPermission::Read, 0, {A.begin(), A.end()}},
      {0x2000, MemoryPermission::Write, 0, std::vector<uint8_t>(9, 0xa5)},
      {0x2009, MemoryPermission::Read, 0, {0x5a}},
  });
  ASSERT_EQ(
      executePortableX87RuntimeStepV1(State, *Memory, Load80, 0x4000, 0x1000)
          .Status,
      PortableX87StepStatusV1::Completed);
  const auto Prefix = State;
  const auto Failed =
      executePortableX87RuntimeStepV1(State, *Memory, Store80, 0x4002, 0x2000);
  EXPECT_EQ(Failed.Status, PortableX87StepStatusV1::MemoryFault);
  EXPECT_FALSE(Failed.InstructionCompleted);
  EXPECT_FALSE(Failed.HasScalarResult);
  ASSERT_TRUE(Failed.MemoryAccess);
  ASSERT_TRUE(Failed.MemoryAccess->Fault);
  EXPECT_EQ(Failed.MemoryAccess->Fault->Kind,
            RuntimeMemoryFaultKindV1::PermissionDenied);
  EXPECT_EQ(Failed.MemoryAccess->Fault->Exit.AccessWidthBits, 80u);
  EXPECT_EQ(Failed.MemoryAccess->Fault->Exit.Address, 0x2000u);
  expectUnchanged(Prefix, State);
  EXPECT_EQ(address(State, 8), 0x4000u);
  const auto Regions = Memory->snapshotMemoryRegions();
  EXPECT_EQ(Regions[1].Bytes, std::vector<uint8_t>(9, 0xa5));
  EXPECT_EQ(Regions[2].Bytes[0], 0x5a);
}

TEST(X87PortableRuntime, ExecutableStoreCommitsPopBeforeInvalidationExit) {
  const auto A = binary80(0x8000000000000001, 0x3fff);
  for (const auto Policy :
       {CodeInvalidationPolicy::RejectExecutableWrites,
        CodeInvalidationPolicy::InvalidateOnExecutableWrite}) {
    SCOPED_TRACE(static_cast<unsigned>(Policy));
    auto State = environment();
    auto Memory = memory(
        {
            {0x1000, MemoryPermission::Read, 0, {A.begin(), A.end()}},
            {0x2000, MemoryPermission::Write | MemoryPermission::Execute, 7,
             std::vector<uint8_t>(10, 0xa5)},
        },
        Policy);
    ASSERT_EQ(
        executePortableX87RuntimeStepV1(State, *Memory, Load80, 0x4000, 0x1000)
            .Status,
        PortableX87StepStatusV1::Completed);
    const auto Prefix = State;
    const auto Result = executePortableX87RuntimeStepV1(State, *Memory, Store80,
                                                        0x4002, 0x2000);
    if (Policy == CodeInvalidationPolicy::RejectExecutableWrites) {
      EXPECT_EQ(Result.Status, PortableX87StepStatusV1::MemoryFault);
      EXPECT_FALSE(Result.InstructionCompleted);
      ASSERT_TRUE(Result.MemoryAccess);
      ASSERT_TRUE(Result.MemoryAccess->Fault);
      EXPECT_EQ(Result.MemoryAccess->Fault->Kind,
                RuntimeMemoryFaultKindV1::ExecutableWriteRejected);
      EXPECT_EQ(Result.MemoryAccess->Fault->Exit.AccessWidthBits, 80u);
      expectUnchanged(Prefix, State);
      const auto Regions = Memory->snapshotMemoryRegions();
      EXPECT_EQ(Regions[1].Generation, 7u);
      EXPECT_EQ(Regions[1].Bytes, std::vector<uint8_t>(10, 0xa5));
      continue;
    }
    EXPECT_EQ(Result.Status, PortableX87StepStatusV1::SelfModification);
    EXPECT_TRUE(Result.InstructionCompleted);
    ASSERT_TRUE(Result.MemoryAccess);
    ASSERT_TRUE(Result.MemoryAccess->SelfModification);
    EXPECT_EQ(Result.MemoryAccess->SelfModification->Size, 10u);
    EXPECT_EQ(Result.MemoryAccess->SelfModification->OldGeneration, 7u);
    EXPECT_EQ(Result.MemoryAccess->SelfModification->NewGeneration, 8u);
    EXPECT_EQ(word(State, 2) >> 11, 5u);
    EXPECT_EQ(State.Image[4], 0);
    EXPECT_EQ(address(State, 8), 0x4002u);
    EXPECT_EQ(Memory->snapshotMemoryRegions()[1].Bytes,
              std::vector<uint8_t>(A.begin(), A.end()));
  }
}

TEST(X87PortableRuntime, PendingUnknownAndInvalidStateNeverReuseMemoryFaults) {
  auto Memory = memory({});
  ASSERT_EQ(Memory->loadScalar(0xdead, GuestScalarWidth::I64).Status,
            GuestMemoryAccessStatus::Fault);
  auto State = environment();
  State.Image[2] = 0x80;
  const auto Before = State;
  auto Pending =
      executePortableX87RuntimeStepV1(State, *Memory, Load80, 0x4000, 0xdead);
  EXPECT_EQ(Pending.Status, PortableX87StepStatusV1::PendingException);
  EXPECT_FALSE(Pending.InstructionCompleted);
  EXPECT_FALSE(Pending.MemoryAccess);
  expectUnchanged(Before, State);
  State.EnvironmentKnown = 0;
  auto Unknown =
      executePortableX87RuntimeStepV1(State, *Memory, 0xe3db, 0x4000, 0);
  EXPECT_EQ(Unknown.Status, PortableX87StepStatusV1::UnknownEnvironment);
  EXPECT_FALSE(Unknown.InstructionCompleted);
  EXPECT_FALSE(Unknown.MemoryAccess);
  State.EnvironmentKnown = 2;
  auto Invalid =
      executePortableX87RuntimeStepV1(State, *Memory, 0xe3db, 0x4000, 0);
  EXPECT_EQ(Invalid.Status, PortableX87StepStatusV1::InvalidState);
  EXPECT_FALSE(Invalid.InstructionCompleted);
  EXPECT_FALSE(Invalid.MemoryAccess);
}

TEST(X87PortableRuntime, ScalarResultsArePublishedOnlyForCompletedProducers) {
  auto State = environment();
  auto Memory = memory(
      {{0x1000, MemoryPermission::Write, 0, std::vector<uint8_t>(10, 0xa5)}});
  const auto Status =
      executePortableX87RuntimeStepV1(State, *Memory, 0xe0df, 0x4000, 0);
  EXPECT_EQ(Status.Status, PortableX87StepStatusV1::Completed);
  EXPECT_TRUE(Status.HasScalarResult);
  EXPECT_EQ(Status.ScalarResult, 5u << 11);
  const auto Before = State;
  const auto Unsupported =
      executePortableX87RuntimeStepV1(State, *Memory, 0xfad9, 0x4002, 0);
  EXPECT_EQ(Unsupported.Status,
            PortableX87StepStatusV1::UnsupportedInstruction);
  EXPECT_FALSE(Unsupported.HasScalarResult);
  EXPECT_EQ(Unsupported.ScalarResult, 0u);
  expectUnchanged(Before, State);
  const auto Fault =
      executePortableX87RuntimeStepV1(State, *Memory, Store80, 0x4002, 0);
  EXPECT_EQ(Fault.Status, PortableX87StepStatusV1::MemoryFault);
  EXPECT_FALSE(Fault.InstructionCompleted);
  expectUnchanged(Before, State);
  const auto Empty =
      executePortableX87RuntimeStepV1(State, *Memory, Store80, 0x4002, 0x1000);
  EXPECT_EQ(Empty.Status, PortableX87StepStatusV1::StackFault);
  EXPECT_FALSE(Empty.InstructionCompleted);
  EXPECT_FALSE(Empty.HasScalarResult);
  expectUnchanged(Before, State);
  EXPECT_EQ(Memory->snapshotMemoryRegions()[0].Bytes,
            std::vector<uint8_t>(10, 0xa5));
}

TEST(X87PortableRuntime, StateIdentityAndUnknownPayloadCannotAuthorizeAStore) {
  auto Memory = memory(
      {{0x1000, MemoryPermission::Write, 0, std::vector<uint8_t>(10, 0xa5)}});
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto State = environment();
    if (Mutation == 0)
      State.Magic ^= 1;
    else if (Mutation == 1)
      ++State.Version;
    else if (Mutation == 2)
      --State.Size;
    else if (Mutation == 3)
      State.Reserved[0] = 1;
    else if (Mutation == 4)
      State.EnvironmentKnown = 2;
    else
      State.EnvironmentKnown = 0;
    const auto Before = State;
    const auto Result = executePortableX87RuntimeStepV1(State, *Memory, Store80,
                                                        0x4000, 0x1000);
    EXPECT_EQ(Result.Status, Mutation == 5
                                 ? PortableX87StepStatusV1::UnknownEnvironment
                                 : PortableX87StepStatusV1::InvalidState);
    EXPECT_FALSE(Result.InstructionCompleted);
    EXPECT_FALSE(Result.MemoryAccess);
    expectUnchanged(Before, State);
  }
  auto State = environment();
  State.Image[4] = 1u << 5;
  const auto Before = State;
  const auto Unknown =
      executePortableX87RuntimeStepV1(State, *Memory, Store80, 0x4000, 0x1000);
  EXPECT_EQ(Unknown.Status, PortableX87StepStatusV1::UnknownPayload);
  EXPECT_FALSE(Unknown.InstructionCompleted);
  expectUnchanged(Before, State);
  EXPECT_EQ(Memory->snapshotMemoryRegions()[0].Bytes,
            std::vector<uint8_t>(10, 0xa5));
}

TEST(X87PortableRuntime, NonwaitingStatusStoreFaultsDespitePendingException) {
  auto State = environment();
  State.Image[2] = 0x80;
  auto Memory = memory({});
  constexpr uint32_t StoreStatus =
      0x38dd | X87ContextMemory | (0x38u << X87ContextOriginalModRMShift);
  const auto Before = State;
  const auto Result = executePortableX87RuntimeStepV1(
      State, *Memory, StoreStatus, 0x4000, 0x1000);
  EXPECT_EQ(Result.Status, PortableX87StepStatusV1::MemoryFault);
  ASSERT_TRUE(Result.MemoryAccess);
  ASSERT_TRUE(Result.MemoryAccess->Fault);
  EXPECT_EQ(Result.MemoryAccess->Fault->Exit.AccessWidthBits, 16u);
  EXPECT_FALSE(Result.InstructionCompleted);
  expectUnchanged(Before, State);
  const auto Waiting = executePortableX87RuntimeStepV1(
      State, *Memory, StoreStatus | X87ContextWaitPrefix, 0x4000, 0x1000);
  EXPECT_EQ(Waiting.Status, PortableX87StepStatusV1::PendingException);
  EXPECT_FALSE(Waiting.MemoryAccess);
  expectUnchanged(Before, State);
}

} // namespace
