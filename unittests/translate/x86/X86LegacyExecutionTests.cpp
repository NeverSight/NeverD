//===- X86LegacyExecutionTests.cpp - Protected execution contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Instruction authentication, event admission and persistent restart state.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

class X86LegacyExecutionTest : public testing::Test {
protected:
  X86LegacyExecutionState S;
  X86LegacyExecutionOptions O{{X86LegacySegmentProfile::WordStackOffsetFirst,
                               X86AlignmentPriority::BeforePaging,
                               {}},
                              X86LegacyEventProfile::OrderedFrame,
                              false,
                              X86LegacyReturnProfile::OrderedFrame};
  std::unique_ptr<X86PagedMemory> M;

  void SetUp() override {
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x20000)});
    M = llvm::cantFail(X86PagedMemory::create(
        RAM, {.PhysicalAddressBits = 32},
        {.CR0 = 0x80010001, .CR3 = 0x10000, .RFlags = 2}));
    put(0x10000, 0x11007, GuestScalarWidth::I32);
    for (unsigned I = 0; I != 32; ++I)
      put(0x11000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
    S.Core.CPL = 3;
    for (auto &C : S.Core.Segments)
      C = {0, UINT32_MAX, 35, 0xc0f3, false};
    S.Core.Segments[1] = {0, UINT32_MAX, 27, 0xc0fb, false};
    S.Core.GDTRBase = 0x7000;
    S.Core.GDTRLimit = 0xff;
    S.Core.GPR[4] = 0xd000;
    S.Core.GPR[0] = 35;
    S.EIP = 0x1000;
    S.EFlags = 0x10202;
    S.CR2 = 0x12345678;
    S.IDTRBase = 0x6000;
    S.IDTRLimit = 0x7ff;
    S.TR = {0x5800, 0x67, 40, 0x8b, false};
    put(0x7008, 0x00cf9b000000ffffULL);
    put(0x7010, 0x00cf93000000ffffULL);
    put(0x7018, 0x00cffb000000ffffULL);
    put(0x7020, 0x00cff3000000ffffULL);
    put(0x5804, 0xe000, GuestScalarWidth::I32);
    put(0x5808, 16, GuestScalarWidth::I16);
    for (unsigned V = 0; V != 256; ++V)
      put(0x6000 + V * 8,
          0x2000 | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
  }

  void put(uint64_t Address, uint64_t Value,
           GuestScalarWidth Width = GuestScalarWidth::I64) {
    ASSERT_EQ(M->physicalMemory().storeScalar(Address, Width, Value).Status,
              GuestMemoryAccessStatus::Completed);
  }

  uint64_t get(uint64_t Address,
               GuestScalarWidth Width = GuestScalarWidth::I32) {
    auto R = M->physicalMemory().loadScalar(Address, Width);
    EXPECT_EQ(R.Status, GuestMemoryAccessStatus::Completed);
    return R.Value;
  }

  X86LegacyExecutionResult run(llvm::ArrayRef<uint8_t> Bytes) {
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(S.EIP + I, Bytes[I], GuestScalarWidth::I8);
    return llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
  }
};

TEST_F(X86LegacyExecutionTest,
       FaultingSegmentLoadEntersKernelWithRestartFrame) {
  S.Core.GPR[0] = 0;
  S.SSBlocked = S.STIBlocked = true;
  auto R = run({0x8e, 0xd0});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.DeliveredVector, 13);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x2000u);
  EXPECT_EQ(S.Core.GPR[4], 0xdfe8u);
  const uint32_t Frame[]{0, 0x1000, 27, 0x10202, 0xd000, 35};
  for (unsigned I = 0; I != 6; ++I)
    EXPECT_EQ(get(S.Core.GPR[4] + 4 * I), Frame[I]);
  EXPECT_EQ(S.EFlags, 2u);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(S.Core.Segments[2].Selector, 16);
  EXPECT_FALSE(S.STIBlocked);
  EXPECT_FALSE(S.SSBlocked);
}

TEST_F(X86LegacyExecutionTest,
       PersistentShadowsEndOnCompletionAndDeferBeforeRAM) {
  EXPECT_TRUE(run({0x8e, 0xd0}).InstructionCompleted);
  ASSERT_TRUE(S.SSBlocked);
  EXPECT_EQ(S.EFlags & 0x10000, 0u);
  put(0x11018, 0, GuestScalarWidth::I32); // Entire IDT unmapped.
  for (auto Kind : {X86LegacyEventKind::Interrupt, X86LegacyEventKind::NMI,
                    X86LegacyEventKind::InstructionBreakpoint}) {
    const uint8_t V = Kind == X86LegacyEventKind::NMI         ? 2
                      : Kind == X86LegacyEventKind::Interrupt ? 64
                                                              : 1;
    auto R =
        llvm::cantFail(deliverX86LegacyEvent(S, *M, {S.EIP, 0, 0, V, Kind}, O));
    EXPECT_EQ(R.Status, X86LegacyExecutionStatus::EventDeferred);
    EXPECT_EQ(R.DeliveryAttempts, 0u);
  }
  EXPECT_TRUE(run({0xbc, 0x00, 0xc0, 0x00, 0x00}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[4], 0xc000u);
  EXPECT_FALSE(S.SSBlocked);
}

TEST_F(X86LegacyExecutionTest, ConsecutiveSSLoadsAndLSSDoNotExtendShadow) {
  EXPECT_TRUE(run({0x8e, 0xd0}).InstructionCompleted);
  EXPECT_TRUE(S.SSBlocked);
  EXPECT_TRUE(run({0x8e, 0xd0}).InstructionCompleted);
  EXPECT_FALSE(S.SSBlocked);
  S.Core.GPR[7] = 0x9000;
  put(0x9000, 0xb000, GuestScalarWidth::I32);
  put(0x9004, 35, GuestScalarWidth::I16);
  EXPECT_TRUE(run({0x0f, 0xb2, 0x27}).InstructionCompleted);
  EXPECT_FALSE(S.SSBlocked);
  EXPECT_EQ(S.Core.GPR[4], 0xb000u);
}

TEST_F(X86LegacyExecutionTest,
       FetchFaultAndChangedBytesPreserveOriginalInstruction) {
  S.EIP = 0x1fff;
  put(0x1fff, 0x90, GuestScalarWidth::I8);
  put(0x11008, 0, GuestScalarWidth::I32);
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x8e, 0xd0}, O));
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::CodeChanged);
  EXPECT_EQ(S.EIP, 0x1fffu);
  EXPECT_EQ(S.CR2, 0x12345678u);
  put(0x1fff, 0x8e, GuestScalarWidth::I8);
  R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x8e, 0xd0}, O));
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x2000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1fffu);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u);
}

TEST_F(X86LegacyExecutionTest,
       SoftwareVectorsNeverPushArchitecturalErrorCodes) {
  for (unsigned Vector : kX86ErrorCodeVectors) {
    SetUp();
    auto R = run({0xcd, uint8_t(Vector)});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[4], 0xdfecu);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1002u);
    EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x202u);
  }
}

TEST_F(X86LegacyExecutionTest,
       FaultDuringEntryKeepsCR2AndNMIBlockThroughShutdown) {
  put(0x11018, 0, GuestScalarWidth::I32);
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::Shutdown);
  EXPECT_EQ(R.DeliveryAttempts, 3u);
  EXPECT_EQ(S.CR2, 0x6040u);
  EXPECT_TRUE(S.NMIBlocked);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.Core.GPR[4], 0xd000u);
}

TEST_F(X86LegacyExecutionTest,
       TargetLimitFailureKeepsFrameAndFaultRFThroughDoubleFault) {
  S.EFlags = 0x202;
  S.Core.GPR[0] = 0;
  put(0x7040, 0x00409a0000000000ULL); // Ring-zero CS with limit zero.
  put(0x6000 + 13 * 8, 0x2000 | (uint64_t{64} << 16) | (uint64_t{0xee} << 40));
  // A same-ring #DF entry leaves the failed inner-ring frame observable.
  put(0x6000 + 8 * 8, 0x2000 | (uint64_t{27} << 16) | (uint64_t{0xee} << 40));
  auto R = run({0x8e, 0xd0});
  ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.Core.GPR[4], 0xcff0u);
  const uint32_t FailedFrame[]{0, 0x1000, 27, 0x10202, 0xd000, 35};
  for (unsigned I = 0; I != 6; ++I)
    EXPECT_EQ(get(0xdfe8 + 4 * I), FailedFrame[I]);
  EXPECT_EQ(get(0xcff0), 0u);
  EXPECT_EQ(get(0xcffc), 0x10202u);
  EXPECT_EQ(get(0x7040, GuestScalarWidth::I64), 0x00409b0000000000ULL);
}

TEST_F(X86LegacyExecutionTest, CLIAndSTIUseIOPLAndVirtualInterruptFlags) {
  S.EFlags = 0x3002;
  EXPECT_TRUE(run({0xfb}).InstructionCompleted);
  EXPECT_TRUE(S.STIBlocked);
  EXPECT_TRUE(run({0xfb}).InstructionCompleted);
  EXPECT_FALSE(S.STIBlocked);
  EXPECT_TRUE(run({0xfa}).InstructionCompleted);
  EXPECT_EQ(S.EFlags & 0x200, 0u);
  auto Controls = M->controls();
  Controls.CR4 = 2;
  llvm::cantFail(M->bindControls(Controls));
  S.EFlags = 2;
  EXPECT_TRUE(run({0xfb}).InstructionCompleted);
  EXPECT_EQ(S.EFlags, 0x80002u);
  EXPECT_FALSE(S.STIBlocked);
  S.EFlags |= 0x100000;
  auto R = run({0x90});
  EXPECT_EQ(R.DeliveredVector, 13);
  EXPECT_FALSE(R.InstructionCompleted);
}

TEST_F(X86LegacyExecutionTest, RefusesUnsupportedInputWithoutGuestSideEffects) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{},
                                         {0x66},
                                         {0x0f},
                                         {0x90, 0x90},
                                         {0xf0, 0x90},
                                         {0xf0, 0xcf},
                                         {0xca},
                                         {0xca, 0x07},
                                         {0x66, 0x66, 0xcf}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x10000), 0x11007u);
    EXPECT_EQ(S.EIP, 0x1000u);
  }
  O.Events.reset();
  auto R = executeX86LegacyInstruction(S, *M, {0x90}, O);
  EXPECT_FALSE(bool(R));
  if (!R)
    llvm::consumeError(R.takeError());
  EXPECT_EQ(get(0x10000), 0x11007u);
}

TEST_F(X86LegacyExecutionTest, UnboundSTINMIPolicyNeverGuessesEligibility) {
  O.NMIBlockedBySTI.reset();
  S.STIBlocked = true;
  auto R = deliverX86LegacyEvent(S, *M,
                                 {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O);
  EXPECT_FALSE(bool(R));
  if (!R)
    llvm::consumeError(R.takeError());
  EXPECT_TRUE(S.STIBlocked);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_EQ(get(0x10000), 0x11007u);
  O.NMIBlockedBySTI = true;
  R = deliverX86LegacyEvent(S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI},
                            O);
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->Status, X86LegacyExecutionStatus::EventDeferred);
  O.NMIBlockedBySTI = false;
  R = deliverX86LegacyEvent(S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI},
                            O);
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_TRUE(S.NMIBlocked);
}

TEST_F(X86LegacyExecutionTest, BackingFailureIsNotAGuestPageFault) {
  put(0x11018, 0x30007, GuestScalarWidth::I32);
  auto R = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.BackingFault);
  EXPECT_FALSE(R.Exception);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_EQ(S.CR2, 0x12345678u);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_TRUE(S.NMIBlocked);
}

TEST_F(X86LegacyExecutionTest, DebugTrapCompletesBeforeFaultableEntry) {
  S.STIBlocked = true;
  put(0x11018, 0x30007, GuestScalarWidth::I32);
  auto R = run({0xf1});
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1001u);
  EXPECT_EQ(S.EFlags, 0x202u);
  EXPECT_FALSE(S.STIBlocked);
  EXPECT_EQ(S.Core.GPR[4], 0xd000u);
}

TEST_F(X86LegacyExecutionTest, InterruptEntryReturnAndResume) {
  for (bool Wide : {false, true}) {
    SetUp();
    const uint32_t PC = S.EIP, SP = S.Core.GPR[4], Flags = S.EFlags;
    put(0x6010,
        0x2000 | (uint64_t{8} << 16) | (uint64_t(Wide ? 0x8e : 0x86) << 40));
    auto Entry = llvm::cantFail(deliverX86LegacyEvent(
        S, *M, {PC, 0, 0, 2, X86LegacyEventKind::NMI}, O));
    ASSERT_EQ(Entry.Status, X86LegacyExecutionStatus::EventDelivered);
    ASSERT_TRUE(S.NMIBlocked);
    auto Returned = Wide ? run({0xcf}) : run({0x66, 0xcf});
    ASSERT_TRUE(Returned.InstructionCompleted);
    EXPECT_EQ(Returned.Status, X86LegacyExecutionStatus::Completed);
    EXPECT_EQ(S.EIP, PC);
    EXPECT_EQ(S.Core.GPR[4], SP);
    EXPECT_EQ(S.Core.CPL, 3);
    EXPECT_EQ(S.EFlags, Wide ? Flags : Flags & 0xffff);
    EXPECT_FALSE(S.NMIBlocked);
    EXPECT_FALSE(S.STIBlocked);
    EXPECT_FALSE(S.SSBlocked);
    EXPECT_TRUE(run({0x90}).InstructionCompleted);
    EXPECT_EQ(S.EIP, PC + 1);
    EXPECT_EQ(S.EFlags & 0x10000, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, ReturnFrameFaultUnblocksOnlyIRET) {
  for (bool Interrupt : {false, true}) {
    SetUp();
    S.NMIBlocked = true;
    put(0x5804, 0xb000, GuestScalarWidth::I32);
    put(0x11034, 0, GuestScalarWidth::I32);
    auto R = run({uint8_t(Interrupt ? 0xcf : 0xcb)});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, Interrupt ? 0xd008u : 0xd004u);
    EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
    EXPECT_EQ(S.NMIBlocked, !Interrupt);
  }
}

TEST_F(X86LegacyExecutionTest, FarReturnDoesNotReadSelectorSlotPadding) {
  S.Core.GPR[4] = 0xcffa;
  put(0xcffa, 0x3000, GuestScalarWidth::I32);
  put(0xcffe, 27, GuestScalarWidth::I16);
  put(0x11034, 0, GuestScalarWidth::I32);
  auto R = run({0xcb});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.Core.GPR[4], 0xd002u);
  EXPECT_EQ(S.Core.Segments[1].Selector, 27);
  EXPECT_EQ(S.CR2, 0x12345678u);
}

TEST_F(X86LegacyExecutionTest, InterruptReturnReadsOnlyTheOuterStackSelector) {
  auto Entered = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
  ASSERT_EQ(Entered.Status, X86LegacyExecutionStatus::EventDelivered);
  S.Core.GPR[4] = 0xcfee;
  put(0xcfee, 0x3000, GuestScalarWidth::I32);
  put(0xcff2, 27, GuestScalarWidth::I16);
  put(0xcff6, 0x202, GuestScalarWidth::I32);
  put(0xcffa, 0xf000, GuestScalarWidth::I32);
  put(0xcffe, 35, GuestScalarWidth::I16);
  put(0x11034, 0, GuestScalarWidth::I32);
  auto R = run({0xcf});
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.Core.GPR[4], 0xf000u);
  EXPECT_EQ(S.Core.Segments[2].Selector, 35);
  EXPECT_EQ(S.CR2, 0x12345678u);
  EXPECT_FALSE(S.NMIBlocked);
}

TEST_F(X86LegacyExecutionTest, FetchAndAdmissionCannotUnblockNMI) {
  S.NMIBlocked = true;
  put(S.EIP, 0x90, GuestScalarWidth::I8);
  auto Changed = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xcf}, O));
  EXPECT_EQ(Changed.Status, X86LegacyExecutionStatus::CodeChanged);
  EXPECT_TRUE(S.NMIBlocked);
  O.Returns.reset();
  auto Unbound = executeX86LegacyInstruction(S, *M, {0xcf}, O);
  ASSERT_FALSE(bool(Unbound));
  llvm::consumeError(Unbound.takeError());
  EXPECT_TRUE(S.NMIBlocked);
  O.Returns = X86LegacyReturnProfile::OrderedFrame;
  S.EFlags |= 0x4000;
  auto Task = executeX86LegacyInstruction(S, *M, {0xcf}, O);
  ASSERT_FALSE(bool(Task));
  llvm::consumeError(Task.takeError());
  EXPECT_TRUE(S.NMIBlocked);
  S.EFlags &= ~0x4000u;
  put(0x11004, 0, GuestScalarWidth::I32);
  auto Fault = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xcf}, O));
  EXPECT_EQ(Fault.DeliveredVector, 14);
  EXPECT_TRUE(S.NMIBlocked);
}

TEST_F(X86LegacyExecutionTest, ReturnedTFStopsBeforeUnownedDebugProduction) {
  put(0xd000, 0x3000, GuestScalarWidth::I32);
  put(0xd004, 27, GuestScalarWidth::I32);
  put(0xd008, 0x10302, GuestScalarWidth::I32);
  EXPECT_TRUE(run({0xcf}).InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.EFlags, 0x10302u);
  auto Next = executeX86LegacyInstruction(S, *M, {0x90}, O);
  ASSERT_FALSE(bool(Next));
  llvm::consumeError(Next.takeError());
  EXPECT_EQ(S.EIP, 0x3000u);
}

TEST_F(X86LegacyExecutionTest, BackingStopKeepsDescriptorEffectsAndCanRetry) {
  auto Entered = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
  ASSERT_EQ(Entered.Status, X86LegacyExecutionStatus::EventDelivered);
  S.Core.GPR[4] = 0xcff4;
  put(0xcff4, 0x3000, GuestScalarWidth::I32);
  put(0xcff8, 27, GuestScalarWidth::I32);
  put(0xcffc, 0x10202, GuestScalarWidth::I32);
  put(0xd000, 0xf000, GuestScalarWidth::I32);
  put(0xd004, 35, GuestScalarWidth::I32);
  put(0x7018, 0x00cffa000000ffffULL);
  put(0x11034, 0x30007, GuestScalarWidth::I32);
  S.STIBlocked = S.SSBlocked = true;
  auto R = run({0xcf});
  EXPECT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(R.InstructionAttempted);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_FALSE(R.Exception);
  EXPECT_TRUE(R.BackingFault);
  EXPECT_EQ(S.EIP, 0x2000u);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(S.Core.GPR[4], 0xcff4u);
  EXPECT_EQ(get(0x7018, GuestScalarWidth::I64), 0x00cffb000000ffffULL);
  EXPECT_FALSE(S.NMIBlocked);
  EXPECT_TRUE(S.STIBlocked);
  EXPECT_TRUE(S.SSBlocked);
  EXPECT_EQ(S.CR2, 0x12345678u);
  put(0x11034, 0xd007, GuestScalarWidth::I32);
  R = run({0xcf});
  EXPECT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.Core.GPR[4], 0xf000u);
  EXPECT_EQ(S.EFlags, 0x10202u);
  EXPECT_FALSE(S.STIBlocked);
  EXPECT_FALSE(S.SSBlocked);
}

TEST_F(X86LegacyExecutionTest, VM86ReturnRequiresItsOwnStateAfterFrameRead) {
  auto Entered = llvm::cantFail(deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
  ASSERT_EQ(Entered.Status, X86LegacyExecutionStatus::EventDelivered);
  put(S.Core.GPR[4] + 8, 0x20202, GuestScalarWidth::I32);
  put(S.EIP, 0xcf, GuestScalarWidth::I8);
  auto R = executeX86LegacyInstruction(S, *M, {0xcf}, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("VM86"), std::string::npos);
  EXPECT_EQ(S.EIP, 0x2000u);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_FALSE(S.NMIBlocked);
}

TEST_F(X86LegacyExecutionTest, UnownedTransitionsPropagateCheckedErrors) {
  put(0x6010, (uint64_t{40} << 16) | (uint64_t{0x85} << 40));
  auto Task = deliverX86LegacyEvent(
      S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O);
  ASSERT_FALSE(bool(Task));
  EXPECT_NE(llvm::toString(Task.takeError()).find("task-switch"),
            std::string::npos);
  EXPECT_TRUE(S.NMIBlocked);
  EXPECT_EQ(S.EIP, 0x1000u);
  put(0x7040, 0x00eff3000000ffffULL); // Reserved data descriptor L bit.
  S.Core.GPR[0] = 67;
  put(0x1000, 0xd88e, GuestScalarWidth::I16);
  auto Data = executeX86LegacyInstruction(S, *M, {0x8e, 0xd8}, O);
  ASSERT_FALSE(bool(Data));
  llvm::consumeError(Data.takeError());
  EXPECT_EQ(S.Core.Segments[3].Selector, 35);
}

} // namespace
