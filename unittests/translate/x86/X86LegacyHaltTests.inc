//===- X86LegacyHaltTests.inc - Protected halt and wake contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// HLT completion, fault priority, idle calls and retained event acceptance.
//===----------------------------------------------------------------------===//

class X86LegacyHaltTest : public X86LegacyExecutionTest {
protected:
  void kernel(bool Code32 = true) {
    resetLegacyState();
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, uint16_t(Code32 ? 0xc09b : 0x809b),
                          false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
  }
};

TEST_F(X86LegacyHaltTest, CompletionAndIdleDoNotFetchFallthrough) {
  for (bool Code32 : {false, true})
    for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0xf4},
                                             {0x66, 0xf4},
                                             {0x67, 0xf4},
                                             {0x64, 0xf4},
                                             {0xf2, 0xf4},
                                             {0xf3, 0xf4},
                                             {0x2e, 0x67, 0x66, 0xf3, 0xf4}}) {
      kernel(Code32);
      S.EIP = 0x2000 - Bytes.size();
      S.STIBlocked = S.SSBlocked = true;
      const auto Before = S;
      put(0x11008, 0, GuestScalarWidth::I32);
      const auto R = run(Bytes);
      ASSERT_EQ(R.Status, X86LegacyExecutionStatus::Halted);
      EXPECT_TRUE(R.InstructionAttempted);
      EXPECT_TRUE(R.InstructionCompleted);
      EXPECT_TRUE(S.Halted);
      EXPECT_EQ(S.EIP, 0x2000u);
      EXPECT_EQ(S.EFlags, Before.EFlags & ~0x10000u);
      EXPECT_EQ(S.Core.GPR, Before.Core.GPR);
      EXPECT_FALSE(S.STIBlocked);
      EXPECT_FALSE(S.SSBlocked);
      const auto RAM = M->physicalMemory().snapshotMemoryRegions();
      // No instruction extent is needed to remain halted. The next code
      // page is absent, so fetching it would incorrectly manufacture #PF.
      const auto Wait =
          llvm::cantFail(executeX86LegacyInstruction(S, *M, {}, O));
      EXPECT_EQ(Wait.Status, X86LegacyExecutionStatus::Halted);
      EXPECT_FALSE(Wait.InstructionAttempted);
      EXPECT_FALSE(Wait.InstructionCompleted);
      EXPECT_FALSE(Wait.Exception);
      EXPECT_EQ(S.EIP, 0x2000u);
      EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
                RAM.front().Bytes);
    }
}

TEST_F(X86LegacyHaltTest, PrivilegeAndLockFaultsRestartAtHalt) {
  for (bool Code32 : {false, true})
    for (unsigned CPL = 0; CPL != 4; ++CPL)
      for (bool Lock : {false, true}) {
        kernel(Code32);
        S.Core.CPL = CPL;
        S.Core.Segments[1].Selector = 24 | CPL;
        S.Core.Segments[1].Attributes |= CPL << 5;
        S.Core.Segments[2].Selector = 32 | CPL;
        S.Core.Segments[2].Attributes |= CPL << 5;
        S.EFlags = 0x13202; // IOPL=3 never authorizes HLT outside CPL0.
        const auto R = run(Lock ? std::vector<uint8_t>{0xf0, 0xf4}
                                : std::vector<uint8_t>{0xf4});
        if (!CPL && !Lock) {
          EXPECT_EQ(R.Status, X86LegacyExecutionStatus::Halted);
          continue;
        }
        ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
        EXPECT_EQ(R.DeliveredVector, Lock ? 6 : 13);
        EXPECT_TRUE(R.InstructionAttempted);
        EXPECT_FALSE(R.InstructionCompleted);
        EXPECT_FALSE(S.Halted);
        EXPECT_EQ(get(S.Core.GPR[4] + (Lock ? 0 : 4)), 0x1000u);
      }
}

TEST_F(X86LegacyHaltTest, FetchFaultPrecedesPrivilegeAndLockChecks) {
  for (bool Lock : {false, true}) {
    resetLegacyState();
    put(0x11004, 0, GuestScalarWidth::I32);
    const auto R = run(Lock ? std::vector<uint8_t>{0xf0, 0xf4}
                            : std::vector<uint8_t>{0xf4});
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, 0x1000u);
    EXPECT_FALSE(S.Halted);
    EXPECT_FALSE(R.InstructionCompleted);
  }
}

TEST_F(X86LegacyHaltTest, MaskedEventsKeepHaltWithoutTableAccess) {
  kernel();
  S.EFlags = 2;
  ASSERT_TRUE(run({0xf4}).InstructionCompleted);
  put(0x11018, 0, GuestScalarWidth::I32);
  S.NMIBlocked = true;
  const auto RAM = M->physicalMemory().snapshotMemoryRegions();
  for (bool NMI : {false, true}) {
    const auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M,
        {S.EIP, 0, 0, uint8_t(NMI ? 2 : 64),
         NMI ? X86LegacyEventKind::NMI : X86LegacyEventKind::Interrupt},
        O));
    EXPECT_EQ(R.Status, X86LegacyExecutionStatus::EventDeferred);
    EXPECT_FALSE(R.EventAccepted);
    EXPECT_EQ(R.DeliveryAttempts, 0u);
    EXPECT_TRUE(S.Halted);
  }
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.front().Bytes);
}

TEST_F(X86LegacyHaltTest, WakeFrameReturnsAfterHalt) {
  for (bool NMI : {false, true}) {
    kernel();
    S.EFlags = NMI ? 2 : 0x202;
    ASSERT_EQ(run({0xf4}).Status, X86LegacyExecutionStatus::Halted);
    const auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M,
        {S.EIP, 0, 0, uint8_t(NMI ? 2 : 64),
         NMI ? X86LegacyEventKind::NMI : X86LegacyEventKind::Interrupt},
        O));
    EXPECT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(R.EventAccepted);
    EXPECT_FALSE(R.InstructionAttempted);
    EXPECT_FALSE(S.Halted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1001u);
    EXPECT_EQ(run({0xcf}).Status, X86LegacyExecutionStatus::Completed);
    EXPECT_EQ(S.EIP, 0x1001u);
    EXPECT_FALSE(S.Halted);
    EXPECT_FALSE(S.NMIBlocked);
  }
}

TEST_F(X86LegacyHaltTest, AcceptedWakeSurvivesDeliveryBudgetAndBackingStop) {
  for (bool Backing : {false, true}) {
    kernel();
    ASSERT_EQ(run({0xf4}).Status, X86LegacyExecutionStatus::Halted);
    O.EventDelivery.MaxAttempts = 1;
    put(0x11018, Backing ? 0x21007 : 0, GuestScalarWidth::I32);
    const auto R = llvm::cantFail(deliverX86LegacyEvent(
        S, *M, {S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}, O));
    EXPECT_EQ(R.Status, Backing
                            ? X86LegacyExecutionStatus::BackingFault
                            : X86LegacyExecutionStatus::EventBudgetExhausted);
    EXPECT_TRUE(R.EventAccepted);
    EXPECT_FALSE(S.Halted);
    EXPECT_TRUE(S.NMIBlocked);
    ASSERT_TRUE(S.PendingEvent);
    auto Step = executeX86LegacyInstruction(S, *M, {0x90}, O);
    ASSERT_FALSE(bool(Step));
    llvm::consumeError(Step.takeError());
    put(0x11018, 0x6007, GuestScalarWidth::I32);
    const auto Resumed = llvm::cantFail(resumeX86LegacyEvent(S, *M, O));
    EXPECT_EQ(Resumed.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_FALSE(Resumed.EventAccepted);
    EXPECT_FALSE(Resumed.InstructionAttempted);
    EXPECT_FALSE(S.Halted);
    EXPECT_FALSE(S.PendingEvent);
    EXPECT_EQ(get(S.Core.GPR[4] + (Backing ? 0 : 4)), 0x1001u);
  }
}

TEST_F(X86LegacyHaltTest,
       SingleStepWakesAtFallthroughAndBreakpointPrecedesHalt) {
  for (bool Breakpoint : {false, true}) {
    kernel();
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::ExecuteBreakpoints;
    S.EFlags = Breakpoint ? 0x202 : 0x10302;
    if (Breakpoint) {
      S.Debug->DR[0] = S.EIP;
      S.Debug->DR7 |= 1;
    }
    const auto R = run({0xf4});
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(R.DeliveredVector, 1);
    EXPECT_EQ(R.InstructionCompleted, !Breakpoint);
    EXPECT_FALSE(S.Halted);
    EXPECT_EQ(get(S.Core.GPR[4]), Breakpoint ? 0x1000u : 0x1001u);
    EXPECT_EQ(S.Debug->DR6 & 0x400f, Breakpoint ? 1u : 0x4000u);
  }
}

TEST_F(X86LegacyHaltTest, InvalidWakeAndIncoherentHaltRefuseWithoutEffects) {
  kernel();
  ASSERT_EQ(run({0xf4}).Status, X86LegacyExecutionStatus::Halted);
  const auto RAM = M->physicalMemory().snapshotMemoryRegions();
  for (const auto E :
       {X86LegacyEvent{0x1000, 0, 0, 64, X86LegacyEventKind::Interrupt},
        X86LegacyEvent{0x1001, 0, 0, 64, X86LegacyEventKind::Software},
        X86LegacyEvent{0x1001, 0, 0, 13, X86LegacyEventKind::Fault},
        X86LegacyEvent{0x1001, 0, 0, 1,
                       X86LegacyEventKind::InstructionBreakpoint}}) {
    auto R = deliverX86LegacyEvent(S, *M, E, O);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_TRUE(S.Halted);
  }
  S.STIBlocked = true;
  auto R = executeX86LegacyInstruction(S, *M, {}, O);
  EXPECT_FALSE(bool(R));
  if (!R)
    llvm::consumeError(R.takeError());
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.front().Bytes);
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0xf4, 0x90},
                                           {0x66, 0x66, 0xf4},
                                           {0xf0, 0xf3, 0xf4},
                                           {0xf3}}) {
    auto Error = validateX86LegacyInstructionEncoding(Bytes, true);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(X86LegacyHaltTest, HaltDoesNotTestTheNextInstructionBreakpoint) {
  kernel();
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::ExecuteBreakpoints;
  S.EFlags = 0x202;
  S.Debug->DR[0] = 0x1001;
  S.Debug->DR7 |= 1;
  ASSERT_EQ(run({0xf4}).Status, X86LegacyExecutionStatus::Halted);
  EXPECT_EQ(
      llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x90}, O)).Status,
      X86LegacyExecutionStatus::Halted);
  EXPECT_EQ(S.Debug->DR6 & 15, 0u);
  ASSERT_EQ(llvm::cantFail(
                deliverX86LegacyEvent(
                    S, *M, {S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}, O))
                .Status,
            X86LegacyExecutionStatus::EventDelivered);
  ASSERT_EQ(run({0xcf}).Status, X86LegacyExecutionStatus::Completed);
  const auto R = run({0x90});
  EXPECT_EQ(R.DeliveredVector, 1);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1001u);
  EXPECT_EQ(S.Debug->DR6 & 15, 1u);
}

TEST_F(X86LegacyHaltTest, BranchSteppingAndDelayedDataHaveDistinctCompletion) {
  for (bool Pending : {false, true}) {
    kernel();
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::BranchWatchpoints;
    S.Debug->DebugCTL = kX86BranchTraceFlag;
    S.Debug->DR6 = 0xffff0ff0;
    S.EFlags = 0x302;
    if (Pending) {
      S.Debug->PendingData = 1;
      S.SSBlocked = true;
    }
    const auto R = run({0xf4});
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(R.Status, Pending ? X86LegacyExecutionStatus::EventDelivered
                                : X86LegacyExecutionStatus::Halted);
    EXPECT_EQ(S.Halted, !Pending);
    EXPECT_EQ(S.Debug->DR6 & 0x400f, Pending ? 1u : 0u);
    EXPECT_EQ(S.Debug->DebugCTL, Pending ? 0u : kX86BranchTraceFlag);
    EXPECT_EQ(S.Debug->PendingData, 0u);
  }
}
