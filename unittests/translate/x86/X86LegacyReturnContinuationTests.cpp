//===- X86LegacyReturnContinuationTests.cpp - Retained return frames -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Return aliases, partial access, debug and refusal across backing repair.
//===----------------------------------------------------------------------===//

#include "X86LegacyVM86TestState.h"

#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

class X86LegacyReturnContinuationTest : public ::testing::Test,
                                        public X86LegacyVM86TestState {
protected:
  void outer(bool Interrupt) {
    vm86ReturnFrame();
    S.Core.GPR[4] = 0x3fcff4;
    S.NMIBlocked = true;
    put(0x11ff0, 0x11007, GuestScalarWidth::I32);
    put(0x11ff4, 0x21007, GuestScalarWidth::I32);
    put(0x11ff8, 0x1b, GuestScalarWidth::I32);
    put(0x11ffc, Interrupt ? 0x202 : 0xc000, GuestScalarWidth::I32);
  }

  void repairOuter(bool Interrupt) {
    repairMpxBacking();
    put(0x21000, Interrupt ? 0xc000 : 0x23, GuestScalarWidth::I32);
    if (Interrupt)
      put(0x21004, 0x23, GuestScalarWidth::I32);
  }
};

TEST_F(X86LegacyReturnContinuationTest, ProtectedOuterReturnsRetainAliasedIP) {
  for (bool Interrupt : {false, true}) {
    outer(Interrupt);
    const uint8_t Opcode = Interrupt ? 0xcf : 0xcb;
    auto R = vmRun({Opcode});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_EQ(get(0x11ff4), 0x21027u);
    EXPECT_EQ(S.NMIBlocked, !Interrupt);
    ASSERT_TRUE(S.PendingTransfer);
    // Both the original frame and code have already been authenticated.
    put(0x1000, 0xcc, GuestScalarWidth::I8);
    repairOuter(Interrupt);
    R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {Opcode}, O));
    ASSERT_TRUE(R.InstructionCompleted);
    EXPECT_FALSE(R.InstructionAttempted);
    EXPECT_EQ(S.EIP, 0x21007u);
    EXPECT_EQ(S.Core.GPR[4], 0xc000u);
    EXPECT_EQ(S.Core.CPL, 3);
    EXPECT_FALSE(S.PendingTransfer);
  }
}

TEST_F(X86LegacyReturnContinuationTest,
       AccessedWriteDoesNotReplaceSavedSelector) {
  vm86ReturnFrame();
  S.Core.GPR[4] = 0x9ff4;
  S.Core.GDTRBase = 0xffffa5f4;
  S.Core.GDTRLimit = 0xffff;
  // The code descriptor and return frame overlap. Its accessed write changes
  // the saved CS from FA03 to FB03 before the outer SS hits missing backing.
  put(0x9ff4, 0x00cffa030000ffffULL);
  put(0x9ffc, 0x202, GuestScalarWidth::I32);
  put(0x11028, 0x21007, GuestScalarWidth::I32);
  auto R = vmRun({0xcf});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(get(0x9ff8, GuestScalarWidth::I16), 0xfb03u);
  repairMpxBacking();
  put(0x21000, 0xc000, GuestScalarWidth::I32);
  put(0x21004, 0xfc03, GuestScalarWidth::I32);
  put(0x211f4, 0x00cff3000000ffffULL);
  R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_EQ(S.Core.Segments[1].Selector, 0xfa03);
  EXPECT_EQ(S.Core.Segments[2].Selector, 0xfc03);
  EXPECT_EQ(S.EIP, 0xffffu);
  EXPECT_EQ(S.Core.CPL, 3);
}

TEST_F(X86LegacyReturnContinuationTest,
       FetchPauseDoesNotBeginReturnOrUnblockNMI) {
  vm86ReturnFrame();
  S.NMIBlocked = true;
  put(0x11004, 0x21007, GuestScalarWidth::I32);
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xcf}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(S.PendingTransfer);
  EXPECT_TRUE(S.NMIBlocked);
  repairMpxBacking();
  put(0x21000, 0xcf, GuestScalarWidth::I8);
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xcf}, O));
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_FALSE(S.NMIBlocked);
}

TEST_F(X86LegacyReturnContinuationTest, EmptyJournalRetainsAttemptAndNMIState) {
  for (bool Interrupt : {false, true}) {
    vm86State();
    S.NMIBlocked = true;
    put(0x11030, 0x21007, GuestScalarWidth::I32);
    const uint8_t Opcode = Interrupt ? 0xcf : 0xcb;
    auto R = vmRun({Opcode});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_TRUE(R.InstructionAttempted);
    ASSERT_TRUE(S.PendingTransfer);
    EXPECT_EQ(S.NMIBlocked, !Interrupt);
    R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {Opcode}, O));
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_FALSE(R.InstructionAttempted);
    repairMpxBacking();
    put(0x21000, 0x180, GuestScalarWidth::I16);
    put(0x21002, 0x100, GuestScalarWidth::I16);
    put(0x21004, 0x202, GuestScalarWidth::I16);
    R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {Opcode}, O));
    ASSERT_TRUE(R.InstructionCompleted);
    EXPECT_FALSE(R.InstructionAttempted);
    EXPECT_EQ(S.EIP, 0x180u);
    EXPECT_EQ(S.Core.GPR[4], Interrupt ? 0xabcd4006u : 0xabcd4004u);
    EXPECT_EQ(S.NMIBlocked, !Interrupt);
    EXPECT_FALSE(S.PendingTransfer);
  }
}

TEST_F(X86LegacyReturnContinuationTest,
       InvalidResumeLeavesRetainedStateAndRAM) {
  outer(true);
  ASSERT_EQ(vmRun({0xcf}).Status, X86LegacyExecutionStatus::BackingFault);
  const auto Before = S;
  const auto Binding = O;
  const auto Controls = M->controls();
  const auto Features = M->features();
  const auto RAM = M->physicalMemory().snapshotMemoryRegions().front().Bytes;
  const auto Rebind = [&](const X86PagingFeatures &F, const X86PagingState &C) {
    auto Image =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Image.Memory = M->physicalMemory().snapshotMemoryRegions();
    M = llvm::cantFail(X86PagedMemory::create(Image, F, C));
  };
  for (unsigned Case = 0; Case != 9; ++Case) {
    S = Before;
    O = Binding;
    auto C = Controls;
    if (Case == 0)
      ++S.Core.GPR[0];
    if (Case == 1)
      O.Segments.AlignmentPriority = X86AlignmentPriority::AfterPaging;
    if (Case == 2)
      O.Segments.FullLimitChoice = X86FullSegmentLimitChoice::Wrap;
    if (Case == 3)
      S.NMIBlocked = true;
    if (Case == 4)
      C.CR0 ^= 0x10000;
    if (Case == 5)
      O.Returns.reset();
    llvm::cantFail(M->bindControls(C));
    if (Case >= 7) {
      auto F = Features;
      if (Case == 7)
        F.PAT = !F.PAT;
      if (Case == 8)
        F.PhysicalAddressBits = 36;
      Rebind(F, C);
    }
    const uint8_t Opcode = Case == 6 ? 0xcb : 0xcf;
    auto R = executeX86LegacyInstruction(S, *M, {Opcode}, O);
    ASSERT_FALSE(R);
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.PendingTransfer, Before.PendingTransfer);
    EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes, RAM);
  }
  S = Before;
  O = Binding;
  Rebind(Features, Controls);
  auto Event = deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O);
  ASSERT_FALSE(Event);
  llvm::consumeError(Event.takeError());
  EXPECT_EQ(S.PendingTransfer, Before.PendingTransfer);
  repairOuter(true);
  EXPECT_TRUE(vmRun({0xcf}).InstructionCompleted);
}

TEST_F(X86LegacyReturnContinuationTest,
       OwnerRefusalCanBeRetriedWithMissingBinding) {
  vm86ReturnFrame();
  O.Virtual8086.reset();
  S.NMIBlocked = true;
  put(0x1000, 0xcf, GuestScalarWidth::I8);
  auto R = executeX86LegacyInstruction(S, *M, {0xcf}, O);
  ASSERT_FALSE(R);
  llvm::consumeError(R.takeError());
  EXPECT_FALSE(S.PendingTransfer);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_EQ(S.Core.CPL, 0);
  O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
  auto Completed = vmRun({0xcf});
  EXPECT_TRUE(Completed.InstructionCompleted);
  EXPECT_TRUE(Completed.InstructionAttempted);
  EXPECT_EQ(S.Core.CPL, 3);
}

TEST_F(X86LegacyReturnContinuationTest,
       LateGuestFaultRetiresRepeatedBackingAttempt) {
  vm86ReturnFrame();
  S.Core.GPR[4] = 0xaff4;
  put(0xaff4, 0x4000, GuestScalarWidth::I32);
  put(0xaff8, 0x1b, GuestScalarWidth::I32);
  put(0xaffc, 0x202, GuestScalarWidth::I32);
  put(0x1102c, 0x21007, GuestScalarWidth::I32);
  const auto Entry = S;
  ASSERT_EQ(vmRun({0xcf}).Status, X86LegacyExecutionStatus::BackingFault);
  auto R = vmRun({0xcf});
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_FALSE(R.InstructionAttempted);
  repairOuter(true);
  put(0x21004, 0, GuestScalarWidth::I16);
  R = vmRun({0xcf});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 13);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(S.PendingTransfer);
  // A handler repairs the guest frame and restores the faulting return site.
  S.Core = Entry.Core;
  S.EFlags = Entry.EFlags;
  S.EIP = Entry.EIP;
  put(0x21004, 0x23, GuestScalarWidth::I16);
  R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_EQ(S.EIP, 0x4000u);
}

TEST_F(X86LegacyReturnContinuationTest, OwnerRefusalRetiresResumedProgress) {
  vm86ReturnFrame();
  O.Virtual8086.reset();
  put(0x11034, 0x21007, GuestScalarWidth::I32);
  ASSERT_EQ(vmRun({0xcf}).Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingTransfer);
  repairMpxBacking();
  const uint32_t Slots[]{0x100, 0x100, 0x23202, 0xabcd4000, 0x800,
                         0x300, 0x400, 0x500,   0x600};
  for (unsigned I = 0; I != 9; ++I)
    put(0x21000 + 4 * I, Slots[I], GuestScalarWidth::I32);
  auto R = executeX86LegacyInstruction(S, *M, {0xcf}, O);
  ASSERT_FALSE(R);
  llvm::consumeError(R.takeError());
  EXPECT_FALSE(S.PendingTransfer);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_EQ(S.Core.CPL, 0);
  O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
  auto Completed = vmRun({0xcf});
  EXPECT_TRUE(Completed.InstructionCompleted);
  EXPECT_TRUE(Completed.InstructionAttempted);
  EXPECT_EQ(S.Core.CPL, 3);
}

TEST_F(X86LegacyReturnContinuationTest, ResumeDebugTrapOwnsItsOwnPausedEvent) {
  for (bool Step : {false, true}) {
    outer(true);
    S.Debug.emplace();
    O.Debug = Step ? X86LegacyDebugProfile::InstructionStep
                   : X86LegacyDebugProfile::Watchpoints;
    if (Step) {
      S.EFlags |= 0x100;
    } else {
      S.Debug->DR[0] = 0x3fcff4;
      S.Debug->DR7 = 0xf0401;
    }
    put(0x11018, 0x22007, GuestScalarWidth::I32);
    // The saved IP is read before suspension and will not be reread on resume.
    ASSERT_EQ(vmRun({0xcf}).Status, X86LegacyExecutionStatus::BackingFault);
    repairOuter(true);
    // A valid return then reaches #DB's IDT access at unavailable backing.
    auto R = vmRun({0xcf});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_FALSE(R.InstructionAttempted);
    EXPECT_FALSE(S.PendingTransfer);
    ASSERT_TRUE(S.PendingEvent);
    EXPECT_EQ(S.PendingEvent->Event.Vector, 1);
    EXPECT_EQ(S.Debug->DR6 & 15, Step ? 0u : 1u);
    EXPECT_EQ(S.Debug->DR6 & 0x4000, Step ? 0x4000u : 0u);
    // Repair only this independent event's missing physical residency. The
    // completed return must never execute again.
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
    const auto &OldBytes = RAM.Memory.front().Bytes;
    RAM.Memory.push_back(
        {0x22000,
         MemoryPermission::Read | MemoryPermission::Write,
         0,
         {OldBytes.begin() + 0x6000, OldBytes.begin() + 0x7000}});
    M = llvm::cantFail(
        X86PagedMemory::create(RAM, M->features(), M->controls()));
    R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(R.DeliveredVector, 1);
    EXPECT_FALSE(R.InstructionAttempted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x21007u);
  }
}

} // namespace
