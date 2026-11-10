//===- X86LegacyTaskTests.cpp - Protected task execution -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Task publication, private incoming faults and backing continuation.
//===----------------------------------------------------------------------===//

#include "../../../lib/translate/X86/X86LegacyTaskSwitch.h"
#include "X86LegacyTaskTestState.h"
#include "gtest/gtest.h"

#include "neverd/translate/X86/X86LegacySourceExecution.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

class X86LegacyTaskTest : public testing::Test, public X86LegacyTaskTestState {
protected:
  void SetUp() override { taskState(); }
  const std::array<uint8_t, 7> Call{0x9a, 0, 0, 0, 0, 0x30, 0};
};

TEST_F(X86LegacyTaskTest, CallAndNestedIretRestoreBothTSSWidths) {
  for (bool Wide : {false, true})
    for (bool OldWide : {false, true}) {
      SCOPED_TRACE(testing::Message() << Wide << '/' << OldWide);
      taskState(Wide, OldWide);
      const auto Entry = S;
      const auto R = run(Call);
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
      EXPECT_TRUE(R.InstructionAttempted);
      EXPECT_TRUE(R.InstructionCompleted);
      EXPECT_EQ(S.EIP, 0x3000u);
      EXPECT_EQ(S.TR.Selector, 48);
      EXPECT_EQ(S.Core.GPR[0], 0x5100u);
      EXPECT_EQ(S.EFlags, 0x4202u);
      EXPECT_EQ(get(0x8000, GuestScalarWidth::I16), 40u);
      EXPECT_EQ((get(0x7028, GuestScalarWidth::I64) >> 40) & 15,
                OldWide ? 11u : 3u);
      EXPECT_EQ((get(0x7030, GuestScalarWidth::I64) >> 40) & 15,
                Wide ? 11u : 3u);
      EXPECT_EQ(M->controls().CR0 & 8, 8u);
      if (OldWide)
        EXPECT_EQ(get(0x581c), 0x10000u);
      S.NMIBlocked = true;
      const auto Back = run({0xcf});
      ASSERT_EQ(Back.Status, X86LegacyExecutionStatus::Completed);
      EXPECT_FALSE(S.NMIBlocked);
      EXPECT_EQ(S.TR.Selector, Entry.TR.Selector);
      EXPECT_EQ(S.EIP, Entry.EIP + Call.size());
      EXPECT_EQ(S.Core.GPR, Entry.Core.GPR);
      EXPECT_EQ(S.EFlags, Entry.EFlags);
      EXPECT_EQ(get(0x8000 + (Wide ? 36 : 16),
                    Wide ? GuestScalarWidth::I32 : GuestScalarWidth::I16),
                0x202u);
      EXPECT_EQ((get(0x7030, GuestScalarWidth::I64) >> 40) & 15,
                Wide ? 9u : 1u);
    }
}

TEST_F(X86LegacyTaskTest, JumpClearsOldBusyWithoutBacklinkOrNT) {
  auto Jump = Call;
  Jump[0] = 0xea;
  auto R = run(Jump);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_EQ(S.EFlags, 0x202u);
  EXPECT_EQ(get(0x8000, GuestScalarWidth::I16), 0x55aau);
  EXPECT_EQ((get(0x7028, GuestScalarWidth::I64) >> 40) & 15, 9u);
  EXPECT_EQ((get(0x7030, GuestScalarWidth::I64) >> 40) & 15, 11u);
}

TEST_F(X86LegacyTaskTest, GDTAndLDTTaskGatesUseGatePrivilege) {
  for (bool Local : {false, true}) {
    taskState();
    put(0x7030, taskDescriptor(0x8000, 103, 0x89)); // Target DPL zero.
    const uint64_t Gate = (uint64_t{0xe5} << 40) | (uint64_t{48} << 16);
    put(Local ? 0x9038 : 0x7038, Gate);
    S.Core.LDTR = {0x9000, 0xff, 64, 0x82, false};
    auto Bytes = Call;
    Bytes[5] = Local ? 0x3f : 0x3b;
    auto R = run(Bytes);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
    EXPECT_EQ(S.TR.Selector, 48);
    EXPECT_EQ(S.EIP, 0x3000u);
  }
}

TEST_F(X86LegacyTaskTest, BusyAndNotPresentFaultBeforeIncomingPublication) {
  for (uint8_t Attribute : {0xebu, 0x69u}) {
    taskState();
    put(0x7030, taskDescriptor(0x8000, 103, Attribute));
    const auto R = run(Call);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(R.DeliveredVector, Attribute == 0xeb ? 13 : 11);
    EXPECT_EQ(S.TR.Selector, 40);
    EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
    EXPECT_EQ(get(0x5820), 0u);
    EXPECT_FALSE(S.PendingTask);
  }
}

TEST_F(X86LegacyTaskTest, IncomingDataFaultUsesNewTaskFrameAndTSS) {
  put(0x7038, 0x00cf73000000ffffULL); // Nonpresent DPL3 data.
  put(0x8054, 0x3b, GuestScalarWidth::I16);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 11);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(S.Core.GPR[0], 0x5100u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x38u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x14202u);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xc000u);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_TRUE(run({0x90}).InstructionCompleted);
}

TEST_F(X86LegacyTaskTest, IncomingCodeFaultCanUseNewInnerPrivilegeStack) {
  put(0x804c, 0x3b, GuestScalarWidth::I16); // Outside valid descriptors.
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 10);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x3bu);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xc000u);
  EXPECT_FALSE(S.PendingTask);
}

TEST_F(X86LegacyTaskTest, LateCodeLimitFaultRetainsIncomingTask) {
  put(0x7018, 0x0000fb0000002fffULL);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 13);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x3000u);
}

TEST_F(X86LegacyTaskTest, IDTTaskGatePushesErrorOnNewStackAndReturns) {
  for (bool Wide : {false, true}) {
    taskState(Wide);
    put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
    auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O));
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(R.EventAccepted);
    EXPECT_EQ(R.DeliveredVector, 13);
    EXPECT_EQ(S.EIP, 0x3000u);
    EXPECT_EQ(S.Core.GPR[4], Wide ? 0xbffcu : 0xbffeu);
    EXPECT_EQ(get(S.Core.GPR[4],
                  Wide ? GuestScalarWidth::I32 : GuestScalarWidth::I16),
              0x1234u);
    EXPECT_EQ(get(0x5824), 0x10202u);
    EXPECT_EQ(S.EFlags, 0x4202u);
    R = run({0xcf});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.EFlags, 0x10202u);
  }
}

TEST_F(X86LegacyTaskTest, LocalDebugClearsButGlobalAndTaskTrapSurvive) {
  S.Debug->DR7 = 0x7ff;
  put(0x8064, 1, GuestScalarWidth::I16);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(R.DeliveredVector, 1);
  EXPECT_EQ(S.Debug->DR7, 0x6aau);
  EXPECT_EQ(S.Debug->DR6 & 0x8000, 0x8000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x3000u);
}

TEST_F(X86LegacyTaskTest, IncomingTFStartsAfterFirstIncomingInstruction) {
  put(0x8024, 0x302, GuestScalarWidth::I32);
  ASSERT_EQ(run(Call).Status, X86LegacyExecutionStatus::Completed);
  const auto R = run({0x90});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 1);
  EXPECT_EQ(S.Debug->DR6 & 0x4000, 0x4000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x3001u);
}

TEST_F(X86LegacyTaskTest, OutgoingTSSAliasingIncomingReadsSavedImage) {
  put(0x7030, taskDescriptor(0x5800, 103, 0xe9));
  const auto Before = S;
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_EQ(S.EIP, Before.EIP + Call.size());
  EXPECT_EQ(S.Core.GPR, Before.Core.GPR);
  EXPECT_EQ(S.EFlags, Before.EFlags | 0x4000);
  EXPECT_EQ(get(0x5800, GuestScalarWidth::I16), 40u);
}

TEST_F(X86LegacyTaskTest, BackingResumeKeepsOriginalSourceKeyAfterCommit) {
  put(0x7038, taskDescriptor(0x9000, 0x3f, 0x82));
  put(0x8060, 0x38, GuestScalarWidth::I16);
  put(0x804c, 0x0f, GuestScalarWidth::I16);
  put(0x11024, 0x21007, GuestScalarWidth::I32);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingTask);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_TRUE(S.Core.Segments[1].Unusable);
  X86LegacySourceExecution Source(S, *M, O);
  uint64_t Key = 0;
  ASSERT_EQ(Source.select(Key), uint32_t(X86LegacyExecutionStatus::Completed));
  EXPECT_EQ(Key, x86LegacySourceKey(0x1000, true));
  auto Event = deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}, O);
  ASSERT_FALSE(bool(Event));
  llvm::consumeError(Event.takeError());
  ++S.Debug->DR[0];
  auto Changed = executeX86LegacyInstruction(S, *M, Call, O);
  ASSERT_FALSE(bool(Changed));
  llvm::consumeError(Changed.takeError());
  --S.Debug->DR[0];
  repairMpxBacking();
  put(0x21008, 0x00cffb000000ffffULL);
  // A host backing repair does not authorize replaying earlier outgoing stores.
  put(0x5820, 0xdeadbeef, GuestScalarWidth::I32);
  put(0x1000, 0xcc, GuestScalarWidth::I8);
  auto Done = llvm::cantFail(executeX86LegacyInstruction(S, *M, Call, O));
  ASSERT_EQ(Done.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_FALSE(Done.InstructionAttempted);
  EXPECT_TRUE(Done.InstructionCompleted);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_EQ(S.Core.Segments[1].Selector, 15);
  EXPECT_EQ(get(0x5820), 0xdeadbeefu);
}

TEST_F(X86LegacyTaskTest, SameCR3RetainsPAEPDPTEBank) {
  auto RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
  auto Controls = M->controls();
  Controls.CR4 = 0x20;
  Controls.PDPTE = {0x11001, 0, 0, 0};
  M = llvm::cantFail(X86PagedMemory::create(
      RAM, {.PhysicalAddressBits = 32, .PAE = true}, Controls));
  put(0x10000, 0xffffffffffffffffULL); // Deliberately invalid RAM PDPT.
  put(0x11000, 0x12007);
  for (unsigned I = 0; I != 32; ++I)
    put(0x12000 + I * 8, I * 4096 | 7);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_EQ(M->controls().PDPTE, Controls.PDPTE);
}

TEST_F(X86LegacyTaskTest, OldTFIsSavedWithoutTrappingAtIncomingEntry) {
  S.EFlags |= 0x100;
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_EQ(get(0x5824), 0x302u);
  EXPECT_EQ(S.EFlags, 0x4202u);
  EXPECT_EQ(S.Debug->DR6 & 0x4000, 0u);
}

TEST_F(X86LegacyTaskTest, MissingIncomingPagePrecedesOutgoingWrites) {
  put(0x11020, 0, GuestScalarWidth::I32);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x8000u);
  EXPECT_EQ(S.TR.Selector, 40);
  EXPECT_EQ(get(0x5820), 0u);
  EXPECT_EQ((get(0x7030, GuestScalarWidth::I64) >> 40) & 15, 9u);
}

TEST_F(X86LegacyTaskTest, TaskGateErrorStorePrecedesLateTargetLimit) {
  put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
  put(0x7018, 0x0000fb0000002fffULL);
  const auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 8); // #GP during #GP task-gate entry.
  EXPECT_EQ(get(0xbffc), 0x1234u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xbffcu);
}

TEST_F(X86LegacyTaskTest, IDTTaskGateCombinesTaskAndDataDebugCauses) {
  put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
  put(0x8064, 1, GuestScalarWidth::I16);
  S.Debug->DR[0] = 0xbffc;
  S.Debug->DR7 = 0x10402; // Global byte write comparator at new error slot.
  const auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 1);
  EXPECT_EQ(S.Debug->DR6 & 0x8001, 0x8001u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x3000u);
}

TEST_F(X86LegacyTaskTest, IDTTaskGateBackingPauseRetainsAcceptedEvent) {
  put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
  put(0x7038, taskDescriptor(0x9000, 0x3f, 0x82));
  put(0x8060, 0x38, GuestScalarWidth::I16);
  put(0x804c, 0x0f, GuestScalarWidth::I16);
  put(0x11024, 0x21007, GuestScalarWidth::I32);
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.EventAccepted);
  ASSERT_TRUE(S.PendingEvent);
  ASSERT_TRUE(S.PendingTask);
  repairMpxBacking();
  put(0x21008, 0x00cffb000000ffffULL);
  put(0x6068, 0); // Accepted gate must not be reread in the incoming context.
  put(0x5820, 0xdeadbeef, GuestScalarWidth::I32);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(get(0xbffc), 0x1234u);
  EXPECT_EQ(get(0x5820), 0xdeadbeefu);
}

TEST_F(X86LegacyTaskTest, TaskErrorStackBackingPauseWritesAndAdvancesOnce) {
  O.Debug = X86LegacyDebugProfile::InstructionStep;
  put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
  put(0x1102c, 0x21007, GuestScalarWidth::I32);
  unsigned Stores = 0;
  const X86PagedMemory::AccessObserver Observer{
      [](void *Context, const X86LinearAccess &Access, uint64_t Bytes) {
        if (Access.Address == 0xbffc &&
            Access.Kind == MemoryAccessKind::Write) {
          EXPECT_EQ(Bytes, 4u);
          ++*static_cast<unsigned *>(Context);
        }
      },
      &Stores};
  auto Previous = M->exchangeAccessObserver(Observer);
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O));
  M->exchangeAccessObserver(Previous);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.EventAccepted);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(S.Core.GPR[4], 0xc000u);
  EXPECT_EQ(Stores, 0u);
  ASSERT_TRUE(S.PendingTask);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_EQ(S.Core.GPR[4], 0xc000u);
  repairMpxBacking();
  put(0x21ff8, 0xdeadbeef, GuestScalarWidth::I32);
  put(0x6068, 0); // Resume retains the accepted gate and original error code.
  Previous = M->exchangeAccessObserver(Observer);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  M->exchangeAccessObserver(Previous);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_EQ(R.DeliveredVector, 13);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.Core.GPR[4], 0xbffcu);
  EXPECT_EQ(Stores, 1u);
  EXPECT_EQ(get(0x21ffc), 0x1234u);
  EXPECT_EQ(get(0x21ff8), 0xdeadbeefu);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_FALSE(S.PendingEvent);
}

TEST_F(X86LegacyTaskTest, VM86DestinationInstallsCachesAndDispatches) {
  put(0x8024, 0x20202, GuestScalarWidth::I32);
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.Exception);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(S.EFlags, 0x24202u);
  EXPECT_FALSE(S.PendingTask);
  for (const auto &C : S.Core.Segments) {
    EXPECT_EQ(C.Base, uint32_t(C.Selector) * 16);
    EXPECT_EQ(C.Limit, 65535u);
    EXPECT_EQ(C.Attributes, 0xf3);
    EXPECT_FALSE(C.Unusable);
  }
  X86LegacySourceExecution Source(S, *M, O);
  uint64_t Key = 0;
  EXPECT_EQ(Source.select(Key), uint32_t(X86LegacyExecutionStatus::Completed));
  EXPECT_EQ(Key, 0x31b0u);
  put(0x31b0, 0x90, GuestScalarWidth::I8);
  EXPECT_EQ(Source.step(Key, {0x90}),
            uint32_t(X86LegacyExecutionStatus::Completed));
  EXPECT_EQ(S.EIP, 0x3001u);
}

TEST_F(X86LegacyTaskTest, MissingFullLimitBindingRefusesBeforeTaskEffects) {
  O.Segments.FullLimitChoice.reset();
  put(0x8038, 1, GuestScalarWidth::I32);
  put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
  auto R = deliverX86LegacyEvent(
      S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(S.TR.Selector, 40);
  EXPECT_EQ(get(0x5820), 0u);
  EXPECT_EQ((get(0x7030, GuestScalarWidth::I64) >> 40) & 15, 9u);
  EXPECT_FALSE(S.PendingTask);
}

TEST_F(X86LegacyTaskTest, IncomingWordTaskConsumesExplicitUpperHalves) {
  taskState(false);
  for (unsigned I = 0; I != 8; ++I)
    (*O.Tasks->WordGPRHigh)[I] = 0x1000 + I;
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  for (unsigned I = 0; I != 8; ++I)
    EXPECT_EQ(S.Core.GPR[I],
              ((0x1000u + I) << 16) | (I == 4 ? 0xc000u : 0x5100u + I));
  EXPECT_EQ(S.Core.Segments[4].Selector, 0);
  EXPECT_EQ(S.Core.Segments[5].Selector, 0);
  EXPECT_TRUE(S.Core.Segments[4].Unusable);
  EXPECT_TRUE(S.Core.Segments[5].Unusable);
}

TEST_F(X86LegacyTaskTest,
       NewAddressSpaceOwnsIncomingDescriptorsAndFollowingFetch) {
  put(0x801c, 0x13000, GuestScalarWidth::I32);
  put(0x13000, 0x14007, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 32; ++I)
    put(0x14000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
  put(0x1401c, 0x17007, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 16; ++I)
    put(0x17000 + I * 8, get(0x7000 + I * 8, GuestScalarWidth::I64));
  put(0x17018,
      0x0000fa000000ffffULL); // New code16, accessed bit initially clear.
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Completed);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
  EXPECT_EQ(S.Core.Segments[1].Attributes, 0xfbu);
  EXPECT_EQ((get(0x17018, GuestScalarWidth::I64) >> 40) & 15, 11u);
  EXPECT_TRUE(run({0xb8, 0x34, 0x12}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x1234u);
}

TEST_F(X86LegacyTaskTest, TaskFaultCanEnterAnotherTaskWithUnloadedCaches) {
  put(0x804c, 0x3b, GuestScalarWidth::I16); // Incoming #TS on CS.
  for (unsigned I = 0; I != 13; ++I)
    put(0x9000 + I * 8, get(0x8000 + I * 8, GuestScalarWidth::I64));
  put(0x904c, 27, GuestScalarWidth::I16);
  put(0x9020, 0x4000, GuestScalarWidth::I32);
  put(0x7040, taskDescriptor(0x9000, 103, 0x89));
  put(0x6050, (uint64_t{0x85} << 40) | (uint64_t{64} << 16));
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 10);
  EXPECT_EQ(S.TR.Selector, 64);
  EXPECT_EQ(S.EIP, 0x4000u);
  EXPECT_EQ(get(0x9000, GuestScalarWidth::I16), 48u);
  EXPECT_EQ(get(0x8020), 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x38u);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_FALSE(S.PendingEvent);
}

TEST_F(X86LegacyTaskTest, ChangedPAERootCommitsOrFaultsInIncomingTask) {
  for (bool Invalid : {false, true}) {
    taskState();
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
    auto Controls = M->controls();
    Controls.RFlags = S.EFlags;
    Controls.CR4 = 0x20;
    Controls.PDPTE = {0x11001, 0, 0, 0};
    M = llvm::cantFail(X86PagedMemory::create(
        RAM, {.PhysicalAddressBits = 32, .PAE = true}, Controls));
    put(0x10000, 0x11001);
    put(0x11000, 0x12007);
    put(0x13000, Invalid ? 0x14003 : 0x14001);
    put(0x14000, 0x15007);
    for (unsigned I = 0; I != 32; ++I) {
      put(0x12000 + I * 8, I * 4096 | 7);
      put(0x15000 + I * 8, I * 4096 | 7);
    }
    put(0x801c, 0x13000, GuestScalarWidth::I32);
    neverd::translate::detail::X86LegacyTaskRequest Request{
        neverd::translate::detail::X86LegacyTaskReason::Call};
    Request.Selector = 48;
    Request.SavedEIP = 0x1007;
    Request.SavedFlags = S.EFlags;
    auto R = llvm::cantFail(
        neverd::translate::detail::switchLegacyTask(S, *M, Request, Call, O));
    ASSERT_TRUE(R.Committed);
    EXPECT_EQ(S.TR.Selector, 48);
    EXPECT_EQ(S.EIP, 0x3000u);
    EXPECT_EQ(M->controls().CR3, 0x13000u);
    if (Invalid) {
      ASSERT_EQ(R.Status, X86PageWalkStatus::Exception);
      ASSERT_TRUE(R.Exception);
      EXPECT_EQ(R.Exception->Vector, 10);
      EXPECT_EQ(R.Exception->ErrorCode, 0u);
      EXPECT_EQ(M->controls().PDPTE, (std::array<uint64_t, 4>{}));
      EXPECT_TRUE(S.PendingTask);
    } else {
      ASSERT_EQ(R.Status, X86PageWalkStatus::Completed);
      EXPECT_EQ(M->controls().PDPTE,
                (std::array<uint64_t, 4>{0x14001, 0, 0, 0}));
      EXPECT_FALSE(S.PendingTask);
      EXPECT_TRUE(run({0x90}).InstructionCompleted);
    }
  }
}

TEST_F(X86LegacyTaskTest, OutgoingPartialSaveResumesAfterBusyClear) {
  for (bool Return : {false, true}) {
    taskState();
    if (Return)
      ASSERT_TRUE(run(Call).InstructionCompleted);
    const auto PC = S.EIP;
    S.TR.Base = 0x1ffdc;
    put(S.Core.GDTRBase + (S.TR.Selector & 0xfff8),
        taskDescriptor(S.TR.Base, 103, 0x8b));
    put(0x1ffdc, 40, GuestScalarWidth::I16);
    put(0x11080, 0x21007, GuestScalarWidth::I32);
    std::vector<uint8_t> Bytes =
        Return ? std::vector<uint8_t>{0xcf}
               : std::vector<uint8_t>{0xea, 0, 0, 0, 0, 0x30, 0};
    const auto R = run(Bytes);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    ASSERT_TRUE(S.PendingTask);
    EXPECT_EQ(S.EIP, PC);
    EXPECT_EQ(get(0x1fffc), PC + Bytes.size());
    EXPECT_EQ((get(S.Core.GDTRBase + (S.TR.Selector & 0xfff8),
                   GuestScalarWidth::I64) >>
               40) &
                  15,
              9u);
    repairMpxBacking();
    put(0x1fffc, 0xdeadbeef, GuestScalarWidth::I32);
    put(PC, 0xcc, GuestScalarWidth::I8);
    const auto Done =
        llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
    ASSERT_EQ(Done.Status, X86LegacyExecutionStatus::Completed);
    EXPECT_FALSE(Done.InstructionAttempted);
    EXPECT_EQ(get(0x1fffc), 0xdeadbeefu);
    EXPECT_EQ(S.TR.Selector, Return ? 40 : 48);
  }
}

TEST_F(X86LegacyTaskTest, TaskErrorStackFaultPrecedesBadIncomingEIP) {
  for (bool Page : {false, true}) {
    taskState();
    put(0x6068, (uint64_t{0xe5} << 40) | (uint64_t{48} << 16));
    put(0x7018, 0x0000fb0000002fffULL);
    if (Page) {
      put(0x1102c, 0, GuestScalarWidth::I32);
    } else {
      put(0x7020, 0x0040f30000000fffULL);
    }
    O.EventDelivery.MaxAttempts = 1;
    const auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M, {0x1000, 0x1234, 0, 13, X86LegacyEventKind::Fault}, O));
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventBudgetExhausted);
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, Page ? 14 : 12);
    EXPECT_EQ(S.TR.Selector, 48);
    EXPECT_EQ(S.Core.GPR[4], 0xc000u);
    EXPECT_EQ(get(0xbffc), 0u);
    EXPECT_EQ(S.PendingEvent->FaultEIP, 0x3000u);
  }
}

TEST_F(X86LegacyTaskTest, RecoveryTaskPauseAndFaultRetainPriorIncomingContext) {
  put(0x7038, taskDescriptor(0x9000, 0x3f, 0x82));
  put(0x8060, 0x38, GuestScalarWidth::I16);
  put(0x804c, 0x0f, GuestScalarWidth::I16);
  put(0x11024, 0, GuestScalarWidth::I32); // Incoming CS lookup #PF.
  put(0x7040, taskDescriptor(0xc000, 103, 0x89));
  put(0x6070, (uint64_t{0x85} << 40) | (uint64_t{64} << 16));
  put(0x11030, 0x21007, GuestScalarWidth::I32); // Recovery TSS host stop.
  O.EventDelivery.MaxAttempts = 1;
  auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.EventAccepted);
  EXPECT_EQ(S.TR.Selector, 48);
  ASSERT_TRUE(S.PendingTask);
  ASSERT_TRUE(S.PendingEvent);
  put(0x11030, 0, GuestScalarWidth::I32); // Retrying backlink now #PF.
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventBudgetExhausted);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_EQ(S.PendingEvent->Event.Vector, 8);
  EXPECT_EQ(S.PendingEvent->FaultEIP, 0x3000u);
  EXPECT_EQ(S.TR.Selector, 48);
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x14202u);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_FALSE(S.PendingEvent);
}

TEST_F(X86LegacyTaskTest, ShortOutgoingTSSProducesArchitecturalTS) {
  S.TR.Limit = 94;
  const auto R = run(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 10);
  EXPECT_EQ(S.TR.Selector, 40);
  EXPECT_EQ(get(S.Core.GPR[4]), 40u);
  EXPECT_EQ(get(0x5820), 0u);
}

} // namespace
