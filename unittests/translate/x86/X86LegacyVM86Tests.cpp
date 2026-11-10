//===- X86LegacyVM86Tests.cpp - VM86 transitions and execution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// VM86 memory, privileged flags, protected events and complete return frames.
//===----------------------------------------------------------------------===//

#include "X86LegacyVM86TestState.h"
#include "X86PortDevice.h"

#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/translate/X86/X86LegacySourceExecution.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {
class X86LegacyVM86Test : public testing::Test, public X86LegacyVM86TestState {
protected:
  void SetUp() override { vm86State(); }
};

TEST_F(X86LegacyVM86Test, IretdInstallsAllSegmentsAndExecutesWordDefault) {
  vm86ReturnFrame();
  S.NMIBlocked = true;
  auto R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd4000u);
  const std::array<uint16_t, 6> Selectors{0x300, 0x100, 0x800,
                                          0x400, 0x500, 0x600};
  for (unsigned I = 0; I != 6; ++I) {
    EXPECT_EQ(S.Core.Segments[I].Selector, Selectors[I]);
    EXPECT_EQ(S.Core.Segments[I].Base, uint32_t(Selectors[I]) * 16);
    EXPECT_EQ(S.Core.Segments[I].Limit, 65535u);
    EXPECT_EQ(S.Core.Segments[I].Attributes, 0xf3);
  }
  R = vmRun({0xb8, 0x34, 0x12});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x1234u);
  EXPECT_EQ(S.EIP, 0x103u);
}

TEST_F(X86LegacyVM86Test, IntelIretdEntryTruncatesHighIP) {
  vm86ReturnFrame();
  put(S.Core.GPR[4], 0x100100, GuestScalarWidth::I32);
  auto R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.EIP, 0x100u);
  EXPECT_TRUE(vmRun({0x90}).InstructionCompleted);
}

TEST_F(X86LegacyVM86Test, CSOverrideIsWritableButCannotCross64KiB) {
  S.Core.GPR[0] = 0x5678;
  auto R = vmRun({0x2e, 0xa3, 0, 4});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(get(0x1400, GuestScalarWidth::I16), 0x5678u);
  R = vmRun({0x2e, 0xa3, 0xff, 0xff});
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
  EXPECT_EQ(get(S.Core.GPR[4]), 0u);
}

TEST_F(X86LegacyVM86Test, SelectorLoadsDoNotConsultProtectedTables) {
  S.Core.GDTRLimit = 0;
  S.Core.GPR[0] = 0x1234;
  auto R = vmRun({0x8e, 0xd8});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.Segments[3].Base, 0x12340u);
  S.Core.GPR[0] = 0;
  R = vmRun({0x8e, 0xd0});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.Segments[2].Base, 0u);
  EXPECT_FALSE(S.Core.Segments[2].Unusable);
  EXPECT_TRUE(S.SSBlocked);
}

TEST_F(X86LegacyVM86Test, InterruptSavesNineSlotsAndIretdRestoresThem) {
  const auto Before = S;
  auto R = vmRun({0xcd, 0x40});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(S.Core.GPR[4], 0xe000u - 36);
  const uint32_t Expected[]{0x102, 0x100, 0x23202, 0xabcd4000, 0x800,
                            0x300, 0x400, 0x500,   0x600};
  for (unsigned I = 0; I != 9; ++I)
    EXPECT_EQ(get(S.Core.GPR[4] + 4 * I), Expected[I]) << I;
  for (unsigned I : {0u, 3u, 4u, 5u}) {
    EXPECT_EQ(S.Core.Segments[I].Selector, 0);
    EXPECT_TRUE(S.Core.Segments[I].Unusable);
  }
  R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x102u);
  EXPECT_EQ(S.EFlags, Before.EFlags);
  EXPECT_EQ(S.Core.GPR, Before.Core.GPR);
  R = vmRun({0x90});
  EXPECT_TRUE(R.InstructionCompleted);
}

TEST_F(X86LegacyVM86Test, SoftwareIntIOPLTrapDiffersFromInt3) {
  vm86State(0x20202);
  auto R = vmRun({0xcd, 3});
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x100u);
  vm86State(0x20202);
  R = vmRun({0xcc});
  EXPECT_EQ(R.DeliveredVector, 3);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x101u);
}

TEST_F(X86LegacyVM86Test, IretWithinVM86IgnoresNTAndPreservesIOPLAndVM) {
  S.EFlags |= 0x4000;
  put(0xc000, 0x200, GuestScalarWidth::I16);
  put(0xc002, 0x123, GuestScalarWidth::I16);
  put(0xc004, 0x203, GuestScalarWidth::I16);
  auto R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x200u);
  EXPECT_EQ(S.Core.Segments[1].Base, 0x1230u);
  EXPECT_EQ(S.EFlags, 0x23203u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd4006u);
}

TEST_F(X86LegacyVM86Test, DwordIretBelowIOPL3FaultsEvenWithVME) {
  vm86State(0x20202);
  vme();
  auto R = vmRun({0x66, 0xcf});
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
  EXPECT_EQ(R.DeliveredVector, 13);
}

TEST_F(X86LegacyVM86Test, FarCallAndReturnUseParagraphsAndWordStack) {
  auto R = vmRun({0x9a, 0, 2, 0x23, 1});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.Segments[1].Base, 0x1230u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd3ffcu);
  EXPECT_EQ(get(0xbffc, GuestScalarWidth::I16), 0x105u);
  R = vmRun({0xcb});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x105u);
  EXPECT_EQ(S.Core.Segments[1].Base, 0x1000u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd4000u);
}

TEST_F(X86LegacyVM86Test, VirtualFlagsRoundTripPreservesHardwareIF) {
  vm86State(0x20202);
  vme();
  ASSERT_TRUE(vmRun({0xfb}).InstructionCompleted);
  EXPECT_EQ(S.EFlags, 0xa0202u);
  EXPECT_FALSE(S.STIBlocked);
  ASSERT_TRUE(vmRun({0x9c}).InstructionCompleted);
  EXPECT_EQ(get(0xbffe, GuestScalarWidth::I16), 0x3202u);
  ASSERT_TRUE(vmRun({0xfa}).InstructionCompleted);
  EXPECT_EQ(S.EFlags, 0x20202u);
  ASSERT_TRUE(vmRun({0x9d}).InstructionCompleted);
  EXPECT_EQ(S.EFlags, 0xa0202u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd4000u);
}

TEST_F(X86LegacyVM86Test, VirtualPOPFRejectsTFAndVIPBeforeCommitting) {
  for (bool Trap : {false, true}) {
    vm86State(Trap ? 0x20202 : 0x120202);
    vme();
    put(0xc000, Trap ? 0x102 : 0x202, GuestScalarWidth::I16);
    auto R = vmRun({0x9d});
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, 13);
    EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xabcd4000u);
  }
}

TEST_F(X86LegacyVM86Test, VirtualDwordFlagsAndMissingVMETrapBeforeStack) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x66, 0x9c}, {0x66, 0x9d}, {0x9c}, {0x9d}}) {
    vm86State(0x20202);
    if (Bytes.size() == 2)
      vme();
    auto R = vmRun(Bytes);
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, 13);
    EXPECT_FALSE(R.InstructionCompleted);
  }
}

TEST_F(X86LegacyVM86Test, VirtualInterruptConflictUsesVMEAndIOPL) {
  EXPECT_TRUE(x86PendingVirtualInterrupt(3, 1, 0x1a0202));
  EXPECT_TRUE(x86PendingVirtualInterrupt(3, 2, 0x1a0202));
  EXPECT_TRUE(x86PendingVirtualInterrupt(3, 1, 0x1a3202));
  EXPECT_TRUE(x86PendingVirtualInterrupt(3, 2, 0x180202));
  EXPECT_FALSE(x86PendingVirtualInterrupt(3, 1, 0x180202));
  vm86State(0x1a0202);
  vme();
  // A pending virtual interrupt must win over unavailable instruction bytes.
  S.Core.Segments[1].Selector = 0xf000;
  S.Core.Segments[1].Base = 0xf0000;
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x90}, O));
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
}
TEST_F(X86LegacyVM86Test,
       ExtendedReturnFrameBackingFailureKeepsProtectedState) {
  vm86ReturnFrame();
  for (unsigned I = 0; I != 9; ++I)
    put(0xafe0 + I * 4, get(0xd000 + I * 4), GuestScalarWidth::I32);
  S.Core.GPR[4] = 0xafe0;
  S.NMIBlocked = true;
  put(0x1102c, 0x21007, GuestScalarWidth::I32);
  auto R = vmRun({0xcf});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(S.Core.GPR[4], 0xafe0u);
  EXPECT_EQ(S.EFlags, 2u);
  EXPECT_FALSE(S.NMIBlocked);
  repairMpxBacking();
  put(0x21000, 0x600, GuestScalarWidth::I16);
  R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.Segments[5].Base, 0x6000u);
  EXPECT_TRUE(vmRun({0x90}).InstructionCompleted);
}

TEST_F(X86LegacyVM86Test, VmeWordIretRestoresVIFAndRejectsTF) {
  for (bool Trap : {false, true}) {
    vm86State(0x20202);
    vme();
    put(0xc000, 0x180, GuestScalarWidth::I16);
    put(0xc002, 0x100, GuestScalarWidth::I16);
    put(0xc004, Trap ? 0x302 : 0x202, GuestScalarWidth::I16);
    const auto R = vmRun({0xcf});
    if (Trap) {
      ASSERT_TRUE(R.Exception);
      EXPECT_EQ(R.Exception->Vector, 13);
      EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xabcd4000u);
    } else {
      EXPECT_TRUE(R.InstructionCompleted);
      EXPECT_EQ(S.EIP, 0x180u);
      EXPECT_EQ(S.EFlags, 0xa0202u);
      EXPECT_EQ(S.Core.GPR[4], 0xabcd4006u);
    }
  }
}

TEST_F(X86LegacyVM86Test, VmeFrameResumeRetainsIVTBitmapAndCompletedStores) {
  vme();
  S.TR.Limit = 0x200;
  put(0x5866, 0x80, GuestScalarWidth::I16);
  put(0x100, 0x01000180, GuestScalarWidth::I32);
  S.Core.GPR[4] = 0xabcd1002;
  put(0x11020, 0x21007, GuestScalarWidth::I32);
  auto R = vmRun({0x66, 0xcd, 0x40});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.EventAccepted);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(get(0x9000, GuestScalarWidth::I16), 0x3202u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd1002u);
  ASSERT_TRUE(S.PendingEvent);
  put(0x9000, 0x55aa, GuestScalarWidth::I16);
  put(0x100, 0xdeadbeef, GuestScalarWidth::I32);
  put(0x5868, 1, GuestScalarWidth::I8);
  repairMpxBacking();
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_FALSE(R.EventAccepted);
  EXPECT_TRUE(R.SoftwareCompleted);
  EXPECT_EQ(get(0x9000, GuestScalarWidth::I16), 0x55aau);
  EXPECT_EQ(get(0x21ffc, GuestScalarWidth::I16), 0x103u);
  EXPECT_EQ(get(0x21ffe, GuestScalarWidth::I16), 0x100u);
  EXPECT_EQ(S.EIP, 0x180u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd0ffcu);
  EXPECT_FALSE(S.PendingEvent);
}

TEST_F(X86LegacyVM86Test, MissingModeProfileResumesAfterBinding) {
  taskState();
  put(0x8024, 0x20202, GuestScalarWidth::I32);
  O.Virtual8086.reset();
  auto R = run({0x9a, 0, 0, 0, 0, 0x30, 0});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::ModeUnavailable);
  O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
  R = llvm::cantFail(
      executeX86LegacyInstruction(S, *M, {0x9a, 0, 0, 0, 0, 0x30, 0}, O));
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_FALSE(S.PendingTask);
  EXPECT_TRUE(vmRun({0x90}).InstructionCompleted);
}

TEST_F(X86LegacyVM86Test, IncomingTaskErrorStackAlignmentBelongsToNewTask) {
  for (bool PageFirst : {false, true}) {
    taskState();
    O.EventDelivery.MaxAttempts = 1;
    O.Segments.AlignmentPriority = PageFirst
                                       ? X86AlignmentPriority::AfterPaging
                                       : X86AlignmentPriority::BeforePaging;
    auto C = M->controls();
    C.CR0 |= 0x40000;
    llvm::cantFail(M->bindControls(C));
    put(0x8024, 0x63202, GuestScalarWidth::I32);
    put(0x8020, 0x10000,
        GuestScalarWidth::I32); // Error push wins over bad EIP.
    put(0x8038, 0xc003, GuestScalarWidth::I32);
    put(0x8050, 0, GuestScalarWidth::I16);
    if (PageFirst)
      put(0x1102c, 0, GuestScalarWidth::I32);
    put(0x6068, (uint64_t{0x85} << 40) | (uint64_t{48} << 16));
    auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M, {S.EIP, 0, 0, 13, X86LegacyEventKind::Fault}, O));
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, PageFirst ? 14 : 17);
    EXPECT_EQ(S.TR.Selector, 48);
    EXPECT_EQ(S.Core.CPL, 3);
    EXPECT_EQ(S.EIP, 0x10000u);
    EXPECT_EQ(S.Core.GPR[4], 0xc003u);
    EXPECT_EQ(get(0xbfff, GuestScalarWidth::I32), 0u);
    ASSERT_TRUE(S.PendingTask);
  }
}
TEST_F(X86LegacyVM86Test, VmeThirdFrameStoreResumesWithoutRewritingCS) {
  vme();
  S.TR.Limit = 0x200;
  put(0x5866, 0x80, GuestScalarWidth::I16);
  put(0x100, 0x01000180, GuestScalarWidth::I32);
  S.Core.GPR[4] = 0xabcd1004;
  put(0x11020, 0x21007, GuestScalarWidth::I32);
  auto R = vmRun({0xcd, 0x40});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(get(0x9002, GuestScalarWidth::I16), 0x3202u);
  EXPECT_EQ(get(0x9000, GuestScalarWidth::I16), 0x100u);
  put(0x9000, 0x55aa, GuestScalarWidth::I16);
  repairMpxBacking();
  R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(get(0x9000, GuestScalarWidth::I16), 0x55aau);
  EXPECT_EQ(get(0x21ffe, GuestScalarWidth::I16), 0x102u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd0ffeu);
}

TEST_F(X86LegacyVM86Test, PortBitmapAppliesEvenAtIOPL3AndBeforeZeroCount) {
  for (bool Denied : {false, true}) {
    vm86State();
    PagedPortTape Tape;
    O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                                PagedPortTape::transfer, &Tape};
    S.TR.Limit = 0x200;
    S.Core.GPR[1] = 0;
    S.Core.GPR[2] = 0x80;
    put(0x5866, 0x80, GuestScalarWidth::I16);
    put(0x5890, Denied ? 1 : 0, GuestScalarWidth::I8);
    auto R = vmRun({0xf3, 0x6c});
    EXPECT_EQ(R.InstructionCompleted, !Denied);
    EXPECT_TRUE(Tape.Transfers.empty());
    if (Denied) {
      ASSERT_TRUE(R.Exception);
      EXPECT_EQ(R.Exception->Vector, 13);
    } else {
      R = vmRun({0xec});
      EXPECT_TRUE(R.InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0] & 0xff, 0x21u);
      EXPECT_EQ(Tape.Transfers.size(), 1u);
    }
  }
}

TEST_F(X86LegacyVM86Test, ProtectedQueriesUDAndControlAccessGPBeforeOperands) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0x00, 0xc0},
                                         {0x0f, 0x02, 0x06, 0xff, 0xff},
                                         {0x0f, 0x03, 0x06, 0xff, 0xff},
                                         {0x0f, 0x20, 0xc0},
                                         {0x63, 0xc8},
                                         {0x63, 0x0e, 0xff, 0xff}}) {
    vm86State();
    auto C = M->controls();
    C.CR0 |= 0x10;
    llvm::cantFail(M->bindControls(C));
    O.ControlRegisters = X86LegacyControlRegisterBinding{};
    O.DescriptorQueries = X86LegacyDescriptorProfile::TableImage;
    O.SystemSelectors = X86LegacySystemSelectorProfile::ExclusiveTable;
    auto R = vmRun(Bytes);
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, Bytes[1] == 0x20 ? 13 : 6);
    EXPECT_EQ(S.CR2, 0x12345678u);
  }
}

TEST_F(X86LegacyVM86Test, TaskLDTFaultRetainsIncomingVM86Image) {
  taskState();
  O.EventDelivery.MaxAttempts = 1;
  put(0x8024, 0x23202, GuestScalarWidth::I32);
  put(0x8060, 4, GuestScalarWidth::I16);
  put(0x6068, (uint64_t{0x85} << 40) | (uint64_t{48} << 16));
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 13, X86LegacyEventKind::Fault}, O));
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 10);
  EXPECT_EQ(R.Exception->ErrorCode, 5u); // Selector with external-event bit.
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.Core.LDTR.Selector, 4);
  EXPECT_TRUE(S.Core.LDTR.Unusable);
  for (const auto &Segment : S.Core.Segments)
    EXPECT_TRUE(Segment.Unusable);
  EXPECT_EQ(get(0x8024), 0x23202u);
}

TEST_F(X86LegacyVM86Test, TaskGateExitAndNestedIretRestoreVM86) {
  taskState();
  put(0x8024, 0x23202, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x9a, 0, 0, 0, 0, 0x30, 0}).InstructionCompleted);
  ASSERT_EQ(S.Core.CPL, 3);
  ASSERT_TRUE(S.EFlags & 0x20000);
  // A separate monitor task is available while the VM86 task remains busy.
  put(0x7038, taskDescriptor(0xa000, 103, 0x89));
  put(0xa01c, 0x10000, GuestScalarWidth::I32);
  put(0xa020, 0x2000, GuestScalarWidth::I32);
  put(0xa024, 0x202, GuestScalarWidth::I32);
  put(0xa038, 0xc000, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 6; ++I)
    put(0xa048 + I * 4, I == 1 ? 8 : 16, GuestScalarWidth::I16);
  put(0x6200, (uint64_t{0xe5} << 40) | (uint64_t{56} << 16));
  auto R = vmRun({0xcd, 0x40});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(S.TR.Selector, 56);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(S.EIP, 0x2000u);
  EXPECT_EQ(get(0xa000, GuestScalarWidth::I16), 48u);
  EXPECT_EQ(get(0x8020), 0x3002u);
  R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.EIP, 0x3002u);
  EXPECT_EQ(S.Core.Segments[1].Base, 27u * 16);
  EXPECT_TRUE(vmRun({0x90}).InstructionCompleted);
}

TEST_F(X86LegacyVM86Test, TaskAlignmentBindingIsCheckedBeforeTaskWrites) {
  for (bool Invalid : {false, true}) {
    taskState();
    O.EventDelivery.MaxAttempts = 1;
    O.Segments.AlignmentPriority.reset();
    if (Invalid)
      O.Segments.AlignmentPriority = static_cast<X86AlignmentPriority>(255);
    auto C = M->controls();
    C.CR0 |= 0x40000;
    llvm::cantFail(M->bindControls(C));
    put(0x8024, 0x63202, GuestScalarWidth::I32);
    put(0x8038, 0xc003, GuestScalarWidth::I32);
    put(0x8050, 0, GuestScalarWidth::I16);
    put(0x6068, (uint64_t{0x85} << 40) | (uint64_t{48} << 16));
    auto Result = deliverX86LegacyEvent(
        S, *M, {S.EIP, 0, 0, 13, X86LegacyEventKind::Fault}, O);
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(S.TR.Selector, 40);
    EXPECT_EQ(get(0x5820), 0u);
    EXPECT_EQ(get(0x8000, GuestScalarWidth::I16), 0x55aau);
    EXPECT_EQ((get(0x7030, GuestScalarWidth::I64) >> 40) & 15, 9u);
    ASSERT_TRUE(S.PendingEvent);
    O.Segments.AlignmentPriority = X86AlignmentPriority::BeforePaging;
    auto R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
    ASSERT_TRUE(R.Exception);
    EXPECT_EQ(R.Exception->Vector, 17);
    EXPECT_EQ(S.TR.Selector, 48);
    EXPECT_TRUE(S.PendingTask);
  }
}

TEST_F(X86LegacyVM86Test, FarCallResumeKeepsAliasedPointerAndCompletedPush) {
  S.Core.Segments[2] = S.Core.Segments[3] = x86VM86SegmentCache(0);
  S.Core.GPR[4] = 0xabcd1002;
  put(0x1000, 0x01000200, GuestScalarWidth::I32);
  put(0x11000, 0x21007, GuestScalarWidth::I32);
  const std::vector<uint8_t> Call{0xff, 0x1e, 0, 0x10};
  auto R = vmRun(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.InstructionAttempted);
  ASSERT_TRUE(S.PendingTransfer);
  EXPECT_EQ(get(0x1000, GuestScalarWidth::I16), 0x100u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd1002u);
  put(0x1000, 0x55aa, GuestScalarWidth::I16);
  put(0x1100, 0xcc, GuestScalarWidth::I8);
  repairMpxBacking();
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Call, O));
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_EQ(S.EIP, 0x200u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd0ffeu);
  EXPECT_EQ(get(0x1000, GuestScalarWidth::I16), 0x55aau);
  EXPECT_EQ(get(0x21ffe, GuestScalarWidth::I16), 0x104u);
  EXPECT_FALSE(S.PendingTransfer);
}

TEST_F(X86LegacyVM86Test,
       ProtectedExternalAlignmentFaultHasZeroErrorAndResumes) {
  for (auto Priority : {X86AlignmentPriority::BeforePaging,
                        X86AlignmentPriority::AfterPaging}) {
    resetLegacyState();
    O.EventDelivery.MaxAttempts = 1;
    O.Segments.AlignmentPriority = Priority;
    S.EFlags = 0x40202;
    S.Core.GPR[4] = 0xd001;
    auto C = M->controls();
    C.CR0 |= 0x40000;
    llvm::cantFail(M->bindControls(C));
    put(0x6200, 0x2000 | (uint64_t{27} << 16) | (uint64_t{0xee} << 40));
    auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M, {S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}, O));
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventBudgetExhausted);
    ASSERT_TRUE(S.PendingEvent);
    EXPECT_EQ(S.PendingEvent->Event.Vector, 17);
    EXPECT_EQ(S.PendingEvent->Event.ErrorCode, 0u);
    R = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(R.DeliveredVector, 17);
    EXPECT_EQ(get(S.Core.GPR[4]), 0u);
    EXPECT_FALSE(S.PendingEvent);
  }
}

TEST_F(X86LegacyVM86Test, FarCallResumeRetainsCompletedStoreWatchpoint) {
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::BranchWatchpoints;
  S.Debug->DR[0] = 0x9000;
  S.Debug->DR7 = 0x50401;
  S.Core.GPR[4] = 0xabcd1002;
  put(0x11020, 0x21007, GuestScalarWidth::I32);
  const std::vector<uint8_t> Call{0x9a, 0x80, 1, 0, 1};
  auto R = vmRun(Call);
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(S.PendingTransfer);
  repairMpxBacking();
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, Call, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 1);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x180u);
  EXPECT_EQ(S.Debug->DR6 & 15, 1u);
  EXPECT_FALSE(S.PendingTransfer);
}

TEST_F(X86LegacyVM86Test, WordIretClearsEntryResumeFlagWithAndWithoutVME) {
  for (bool LowIOPL : {false, true}) {
    vm86State(LowIOPL ? 0x30202 : 0x33202);
    if (LowIOPL)
      vme();
    put(0xc000, 0x180, GuestScalarWidth::I16);
    put(0xc002, 0x100, GuestScalarWidth::I16);
    put(0xc004, 0x202, GuestScalarWidth::I16);
    auto R = vmRun({0xcf});
    ASSERT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(S.EFlags & 0x10000, 0u);
    EXPECT_EQ(S.EIP, 0x180u);
  }
}

TEST_F(X86LegacyVM86Test, IretdResumeRetainsIPAliasedByPageAccessedBit) {
  vm86ReturnFrame();
  S.Core.GPR[4] = 0x3fcff4;
  S.NMIBlocked = true;
  // The frame starts in its own last four PTEs. Reading the outer SS uses
  // the next PTE, which is also the IP already read by this same IRETD.
  put(0x11ff0, 0x11007, GuestScalarWidth::I32);
  put(0x11ff4, 0x21007, GuestScalarWidth::I32);
  put(0x11ff8, 0x100, GuestScalarWidth::I32);
  put(0x11ffc, 0x23202, GuestScalarWidth::I32);
  auto R = vmRun({0xcf});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_EQ(get(0x11ff4), 0x21027u);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.Core.GPR[4], 0x3fcff4u);
  repairMpxBacking();
  const uint32_t Tail[]{0xabcd4000, 0x800, 0x300, 0x400, 0x500, 0x600};
  for (unsigned I = 0; I != 6; ++I)
    put(0x21000 + 4 * I, Tail[I], GuestScalarWidth::I32);
  R = vmRun({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_FALSE(R.InstructionAttempted);
  EXPECT_EQ(S.EIP, 0x1007u);
  EXPECT_EQ(S.Core.GPR[4], 0xabcd4000u);
  EXPECT_EQ(S.Core.Segments[5].Selector, 0x600);
}

} // namespace
