//===- X64PagedEventDeliveryTests.cpp - Nested architectural faults -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Restart metadata, descriptor faults, retained RAM effects and #DF shutdown.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace neverd::analysis;
using namespace neverd::translate;

namespace {

class X64PagedEventTest : public testing::Test {
protected:
  X64ContinuationStateV6 State;
  GuestState RAM;
  X86PagingFeatures Features{.PhysicalAddressBits = 48,
                             .PAE = true,
                             .LongMode = true,
                             .PAT = true,
                             .NX = true};
  X86PagingState Controls{.CR0 = 0x80010001,
                          .CR3 = 0x1000,
                          .CR4 = 0x20,
                          .EFER = 0xd00,
                          .RFlags = 0x202};
  std::unique_ptr<X86PagedMemory> Memory;

  void SetUp() override {
    RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x20000)});
    put(0x1000, 0x2007);
    put(0x2000, 0x3007);
    put(0x3000, 0x4007);
    for (unsigned I = 0; I != 32; ++I)
      put(0x4000 + I * 8, I * 4096 | 3);
    put(0x5008, 0x00af9b000000ffffULL);
    put(0x5010, 0x00cf93000000ffffULL);
    State.Core.CS = {0, UINT32_MAX, 8, 0xa09b};
    State.Core.SS = {0, UINT32_MAX, 16, 0xc093};
    State.GDTR = {0x5000, 0x3f};
    State.IDTR = {0x6000, 4095};
    State.TR = {0x8000, 0x67, 40, 0x8b};
    State.Core.Core.Core.Core.RFlags = 0x202;
    State.Core.Core.Core.Core.PC = 0x9000;
    State.Core.Core.Core.Core.GPR[4] = 0x1e000;
    for (unsigned V = 0; V != 256; ++V)
      gate(V);
  }

  void put(uint64_t Address, uint64_t Value) {
    ASSERT_LE(Address + 8, RAM.Memory.front().Bytes.size());
    for (unsigned I = 0; I != 8; ++I)
      RAM.Memory.front().Bytes[Address + I] = Value >> (8 * I);
  }

  void gate(unsigned Vector, uint8_t Access = 0x8e, unsigned IST = 0,
            unsigned Selector = 8) {
    const uint64_t PC = 0x10000 + Vector * 16;
    const uint64_t Address = State.IDTR.Base + Vector * 16;
    put(Address, (PC & 0xffff) | (uint64_t(Selector) << 16) |
                     (uint64_t(IST) << 32) | (uint64_t(Access) << 40) |
                     ((PC & 0xffff0000) << 32));
    put(Address + 8, 0);
  }

  X64PagedEventResult deliver(unsigned Vector, X64ContinuationEventKind Kind,
                              unsigned Error = 0, uint64_t CR2 = 0) {
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64ContinuationEventV1 Event;
    Event.Kind = Kind;
    Event.Vector = Vector;
    Event.SavedPC = State.Core.Core.Core.Core.PC;
    Event.ErrorCode = Error;
    Event.FaultAddress = CR2;
    return llvm::cantFail(deliverX64PagedEvent(State, *Memory, Event, 0xffff));
  }

  uint64_t get(uint64_t Address) {
    return Memory->physicalMemory()
        .loadScalar(Address, GuestScalarWidth::I64)
        .Value;
  }
};

TEST_F(X64PagedEventTest, PageFaultPublishesCR2AndRestartFrame) {
  const auto R = deliver(14, X64ContinuationEventKind::Fault, 7, 0xabcdef);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_EQ(State.Core.Core.CR2, 0xabcdefu);
  EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x100e0u);
  const auto SP = State.Core.Core.Core.Core.GPR[4];
  EXPECT_EQ(SP, 0x1dfd0u);
  EXPECT_EQ(get(SP), 7u);
  EXPECT_EQ(get(SP + 8), 0x9000u);
  EXPECT_EQ(get(SP + 24), 0x10202u);
}

TEST_F(X64PagedEventTest, StackFrameWrapsAcrossTheLinearAddressBoundary) {
  for (uint64_t OldSP : {0u, 16u}) {
    SetUp();
    for (unsigned Level = 0; Level != 4; ++Level)
      put((Level + 1) * 0x1000 + 511 * 8,
          Level == 3 ? 0x1f003 : (Level + 2) * 0x1000 + 3);
    State.Core.Core.Core.Core.GPR[4] = OldSP;
    const auto R = deliver(14, X64ContinuationEventKind::Fault, 7, 0xabcdef);
    ASSERT_EQ(R.Status, X64PagedEventStatus::Delivered);
    EXPECT_EQ(R.DeliveryAttempts, 1u);
    const uint64_t SP = OldSP - 48;
    EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], SP);
    EXPECT_EQ(get(0x1f000 + (SP & 4095)), 7u);
    EXPECT_EQ(get(0x1f000 + ((SP + 8) & 4095)), 0x9000u);
    const uint64_t SavedSP = OldSP - 16;
    EXPECT_EQ(get(SavedSP < 4096 ? SavedSP : 0x1f000 + (SavedSP & 4095)),
              OldSP);
  }
}

TEST_F(X64PagedEventTest, SamePrivilegeEntryDoesNotRereadItsCodeDescriptor) {
  // The descriptor aliases its own PTE. Setting descriptor.A changes that
  // mapping's physical address; the already consumed descriptor must not be
  // loaded again as the unused arm of a stack-switch SELECT.
  const uint64_t Descriptor = 0x00209a0000000003ULL;
  const uint64_t Table = Descriptor & 0x0000fffffffff000ULL;
  put(0x3000, Table | 7);
  put(0x4028, Descriptor);
  State.GDTR.Base = 0x5020;
  RAM.Memory.push_back(
      {Table, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(RAM.Memory.front().Bytes.begin() + 0x4000,
                            RAM.Memory.front().Bytes.begin() + 0x5000)});
  const auto R = deliver(3, X64ContinuationEventKind::Trap);
  ASSERT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 3);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_EQ(get(Table + 0x28), Descriptor | 0x60 | (uint64_t(1) << 40));
}

TEST_F(X64PagedEventTest, SoftwareVectorIdentityDoesNotMakeItContributory) {
  gate(13, 0x0e); // Not present; software INT13 is a benign first event.
  const auto R = deliver(13, X64ContinuationEventKind::Software);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 11);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 13u * 8 + 2);
}

TEST_F(X64PagedEventTest, ContributoryThenContributoryBecomesDoubleFault) {
  gate(13, 0x0e);
  const auto R = deliver(13, X64ContinuationEventKind::Fault);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 0u);
  ASSERT_TRUE(R.LastException);
  EXPECT_EQ(R.LastException->Vector, 11);
  EXPECT_EQ(R.LastException->ErrorCode, 13u * 8 + 3);
}

TEST_F(X64PagedEventTest, GatePrivilegeCheckPrecedesPresenceCheck) {
  State.Core.CS.Selector = 27;
  State.Core.CS.Attributes = 0xa0fb;
  gate(32, 0x0e); // DPL0 and absent; CPL3 INT must report #GP first.
  put(0x8004, 0x1e000);
  const auto R = deliver(32, X64ContinuationEventKind::Software);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 13);
  ASSERT_TRUE(R.LastException);
  EXPECT_EQ(R.LastException->ErrorCode, 32u * 8 + 2);
}

TEST_F(X64PagedEventTest, NestedPageFaultUpdatesCR2BeforeDoubleFault) {
  State.IDTR.Base = 0x6f30;
  for (unsigned V : {8u, 13u, 14u})
    gate(V);
  put(0x4000 + 7 * 8, 0);
  const auto R = deliver(14, X64ContinuationEventKind::Fault, 0, 0xfeed);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(State.Core.Core.CR2, 0x7010u);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 0u);
}

TEST_F(X64PagedEventTest, FaultWhileDeliveringDoubleFaultReportsShutdown) {
  State.IDTR.Base = 0x6f30;
  for (unsigned V : {8u, 13u, 14u})
    gate(V);
  put(0x4000 + 6 * 8, 0);
  put(0x4000 + 7 * 8, 0);
  const auto R = deliver(14, X64ContinuationEventKind::Fault, 0, 0xfeed);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Shutdown);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(State.Core.Core.CR2, 0x6fb0u);
  EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x9000u);
}

TEST_F(X64PagedEventTest,
       PartialFrameStoresSurviveDeliveryFaultAndISTRecovery) {
  State.Core.Core.Core.Core.GPR[4] = 0x1e010;
  put(0x4000 + 0x1d * 8, 0);
  gate(8, 0x8e, 1);
  put(0x8000 + 36, 0x1c000);
  const auto R = deliver(3, X64ContinuationEventKind::Trap);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 3u);
  EXPECT_EQ(State.Core.Core.CR2, 0x1dff8u);
  EXPECT_EQ(get(0x1e008), 16u);
  EXPECT_EQ(get(0x1e000), 0x1e010u);
  EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], 0x1bfd0u);
}

TEST_F(X64PagedEventTest, NMIBlockingPersistsWhenItsGateFaults) {
  gate(2, 0x0e);
  const auto R = deliver(2, X64ContinuationEventKind::NMI);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 11);
  EXPECT_EQ(State.NMIBlocked, 1);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 2u * 8 + 3);
}

TEST_F(X64PagedEventTest, InvalidInputDoesNotPublishCR2OrMemory) {
  const auto Original = State;
  const auto R = deliver(14, X64ContinuationEventKind::Software, 1, 0xfeed);
  EXPECT_EQ(R.Status, X64PagedEventStatus::InvalidState);
  EXPECT_EQ(std::memcmp(&State, &Original, sizeof(State)), 0);
  EXPECT_EQ(get(0x1000), 0x2007u);
}

TEST_F(X64PagedEventTest, BackingFailureIsNotConvertedToDoubleFault) {
  put(0x4000 + 6 * 8, 0x800003);
  const auto R = deliver(13, X64ContinuationEventKind::Fault);
  EXPECT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_FALSE(R.LastException);
  ASSERT_TRUE(R.BackingFault);
  EXPECT_EQ(R.BackingFault->Kind, RuntimeMemoryFaultKindV1::Unmapped);
}

TEST_F(X64PagedEventTest, FREDControlCannotUseLegacyIDTDelivery) {
  Controls.CR4 |= uint64_t(1) << 32;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  const auto Before = State;
  X64ContinuationEventV1 Event;
  auto R = deliverX64PagedEvent(State, *Memory, Event, 0xffff);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
  EXPECT_EQ(get(0x1000), 0x2007u);
}

TEST_F(X64PagedEventTest, AcceptedNMIResumesWithoutRecheckingItsOwnBlocker) {
  put(0x4030, 0x800003);
  const auto R = deliver(2, X64ContinuationEventKind::NMI);
  ASSERT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  ASSERT_TRUE(R.Continuation);
  EXPECT_TRUE(R.InputAccepted);
  EXPECT_EQ(State.NMIBlocked, 1);
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x4030, GuestScalarWidth::I64, 0x6003)
                .Status,
            GuestMemoryAccessStatus::Completed);
  const auto Resumed = llvm::cantFail(
      resumeX64PagedEvent(State, *Memory, *R.Continuation, 0xffff));
  EXPECT_EQ(Resumed.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(Resumed.DeliveredVector, 2);
  EXPECT_TRUE(Resumed.InputAccepted);
  EXPECT_FALSE(Resumed.Continuation);
  EXPECT_EQ(State.NMIBlocked, 1);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 0x9000u);
}

TEST_F(X64PagedEventTest, ResumeRetainsSecondaryFaultAndSoftwareRestartPC) {
  State.IDTR.Base = 0x6f80;
  for (unsigned V = 0; V != 256; ++V)
    gate(V);
  gate(3, 0x0e);
  put(0x4038, 0x800003); // The secondary #NP gate has no backing.
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X64ContinuationEventV1 Event;
  Event.Vector = 3;
  Event.Kind = X64ContinuationEventKind::Software;
  Event.SavedPC = 0x9001;
  const auto R =
      llvm::cantFail(deliverX64PagedEvent(State, *Memory, Event, 0xffff));
  ASSERT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  ASSERT_TRUE(R.Continuation);
  EXPECT_EQ(R.Continuation->Event.Vector, 11);
  EXPECT_EQ(R.Continuation->FaultPC, 0x9000u);
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x4038, GuestScalarWidth::I64, 0x7003)
                .Status,
            GuestMemoryAccessStatus::Completed);
  // Repairing the original gate must not replace the already selected #NP.
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x6fb5, GuestScalarWidth::I8, 0x8e)
                .Status,
            GuestMemoryAccessStatus::Completed);
  const auto Resumed = llvm::cantFail(
      resumeX64PagedEvent(State, *Memory, *R.Continuation, 0xffff));
  EXPECT_EQ(Resumed.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(Resumed.DeliveredVector, 11);
  const auto SP = State.Core.Core.Core.Core.GPR[4];
  EXPECT_EQ(get(SP), 26u);
  EXPECT_EQ(get(SP + 8), 0x9000u);
  EXPECT_EQ(get(SP + 24), 0x10202u);
}

TEST_F(X64PagedEventTest,
       ResumeDoesNotRereadDescriptorOverwrittenByPartialFrame) {
  State.Core.Core.Core.Core.GPR[4] = 0x5020;
  put(0x4020, 0x800003); // Final PC slot crosses into unbacked page four.
  const auto R = deliver(1, X64ContinuationEventKind::Trap);
  ASSERT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  ASSERT_TRUE(R.Continuation);
  EXPECT_EQ(get(0x5008),
            0x202u); // Saved flags have overwritten the CS descriptor.
  EXPECT_EQ(get(0x5010), 0x5020u);
  EXPECT_EQ(get(0x5018), 16u);
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x4020, GuestScalarWidth::I64, 0x14003)
                .Status,
            GuestMemoryAccessStatus::Completed);
  const auto Resumed = llvm::cantFail(
      resumeX64PagedEvent(State, *Memory, *R.Continuation, 0xffff));
  EXPECT_EQ(Resumed.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(Resumed.DeliveredVector, 1);
  EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x10010u);
  EXPECT_EQ(State.Core.CS.Attributes, 0xa09bu);
  EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], 0x4ff8u);
  EXPECT_EQ(get(0x14ff8), 0x9000u);
}

} // namespace
