//===- X86LegacyHaltSourceTests.inc - Compiled halt and wake execution ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The x64 host dispatcher retains guest HLT state and attempt accounting.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacySourceTest, HaltWaitsBeforeUnselectedCodeAndResumesAfterIRET) {
  const std::vector<SourceUnit> Units{{0x1000, 16, {0x66, 0xf4}},
                                      {0x1000, 32, {0x66, 0xf4}},
                                      {0x2000, 32, {0xcf}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  for (bool Code32 : {false, true}) {
    controlState(false, Code32);
    install(Units);
    S.EFlags = 2;
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 2;
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacyExecutionStatus::Halted));
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(S.EIP, 0x1002u);
    EXPECT_TRUE(S.Halted);
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    const auto RAM = M->physicalMemory().snapshotMemoryRegions();
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(Run(&Context, &Budget),
                uint32_t(X86LegacyExecutionStatus::Halted));
      EXPECT_EQ(Budget, 1u);
    }
    EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
              RAM.front().Bytes);
    EXPECT_EQ(Context.deliver({S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}),
              uint32_t(X86LegacyExecutionStatus::EventDeferred));
    EXPECT_TRUE(S.Halted);
    EXPECT_EQ(Context.deliver({S.EIP, 0, 0, 2, X86LegacyEventKind::NMI}),
              uint32_t(X86LegacyExecutionStatus::EventDelivered));
    EXPECT_TRUE(Context.lastResult().EventAccepted);
    EXPECT_FALSE(S.Halted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1002u);
    Budget = 2;
    EXPECT_EQ(Run(&Context, &Budget), uint32_t(X86LegacySourceExit::Unselected))
        << Context.lastError().str();
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(S.EIP, 0x1002u);
    EXPECT_FALSE(S.Halted);
    EXPECT_FALSE(S.NMIBlocked);
  }
}

TEST_P(X86LegacySourceTest, HaltWakeBackingResumeDoesNotRepeatInstruction) {
  const std::vector<SourceUnit> Units{
      {0x1000, 32, {0xf4}}, {0x1001, 32, {0x90}}, {0x2000, 32, {0xcf}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  controlState();
  install(Units);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 4;
  ASSERT_EQ(Run(&Context, &Budget), uint32_t(X86LegacyExecutionStatus::Halted));
  EXPECT_EQ(Budget, 3u);
  put(0x11018, 0x21007, GuestScalarWidth::I32);
  EXPECT_EQ(Context.deliver({S.EIP, 0, 0, 64, X86LegacyEventKind::Interrupt}),
            uint32_t(X86LegacyExecutionStatus::BackingFault));
  EXPECT_TRUE(Context.lastResult().EventAccepted);
  EXPECT_FALSE(S.Halted);
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacySourceExit::PendingEvent));
  EXPECT_EQ(Budget, 3u);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  EXPECT_EQ(Context.resume(),
            uint32_t(X86LegacyExecutionStatus::EventDelivered));
  EXPECT_FALSE(Context.lastResult().EventAccepted);
  EXPECT_EQ(Run(&Context, &Budget), uint32_t(X86LegacySourceExit::Unselected))
      << Context.lastError().str();
  EXPECT_EQ(Budget, 1u); // Original HLT, handler IRET, following NOP.
  EXPECT_EQ(S.EIP, 0x1002u);
  EXPECT_FALSE(S.Halted);
}

TEST_P(X86LegacySourceTest, HaltDebugAndPrivilegeFaultUseSharedStateOwner) {
  const std::vector<SourceUnit> Units{{0x1000, 32, {0xf4}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  for (bool User : {false, true}) {
    if (User)
      resetLegacyState();
    else
      controlState();
    install(Units);
    S.Debug.emplace();
    O.Debug = X86LegacyDebugProfile::InstructionStep;
    S.EFlags = 0x3302;
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacyExecutionStatus::EventDelivered));
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(Context.lastResult().DeliveredVector, User ? 13 : 1);
    EXPECT_EQ(Context.lastResult().InstructionCompleted, !User);
    EXPECT_FALSE(S.Halted);
    EXPECT_EQ(get(S.Core.GPR[4] + (User ? 4 : 0)), User ? 0x1000u : 0x1001u);
  }
}
