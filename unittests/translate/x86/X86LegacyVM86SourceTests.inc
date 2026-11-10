//===- X86LegacyVM86SourceTests.inc - Generated VM86 dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile host-x64 drivers and retain mixed VM86/protected execution state.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacySourceTest, VM86IretdEventAndReturnRedispatch) {
  vm86ReturnFrame();
  const std::vector<SourceUnit> Units{{0x1000, 32, {0xcf}},
                                      {0x1100, 16, {0xb8, 0x34, 0x12}},
                                      {0x1103, 16, {0xcd, 0x40}},
                                      {0x1105, 16, {0x90}},
                                      {0x2000, 32, {0xcf}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 5;
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacyExecutionStatus::EventDelivered));
  EXPECT_EQ(Budget, 2u);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x105u);
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.Core.GPR[0], 0x1234u);
  EXPECT_EQ(S.EIP, 0x106u);
}

TEST_P(X86LegacySourceTest, VM86RedirectionResumesWithoutRepeatingAcceptedINT) {
  vm86State();
  vme();
  S.TR.Limit = 0x200;
  S.Core.GPR[4] = 0xabcd1002;
  put(0x5866, 0x80, GuestScalarWidth::I16);
  put(0x100, 0x01000180, GuestScalarWidth::I32);
  put(0x11020, 0x21007, GuestScalarWidth::I32);
  const std::vector<SourceUnit> Units{{0x1100, 16, {0x66, 0xcd, 0x40}},
                                      {0x1180, 16, {0xb8, 0x78, 0x56}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  uint64_t Budget = 2;
  {
    X86LegacySourceExecution Context(S, *M, O);
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacyExecutionStatus::BackingFault));
    EXPECT_EQ(Budget, 1u);
  }
  put(0x9000, 0x55aa, GuestScalarWidth::I16);
  put(0x100, 0xdeadbeef, GuestScalarWidth::I32);
  repairMpxBacking();
  X86LegacySourceExecution Restored(S, *M, O);
  EXPECT_EQ(Restored.resume(),
            uint32_t(X86LegacyExecutionStatus::EventDelivered));
  EXPECT_EQ(Run(&Restored, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.Core.GPR[0], 0x5678u);
  EXPECT_EQ(S.EIP, 0x183u);
  EXPECT_EQ(get(0x9000, GuestScalarWidth::I16), 0x55aau);
}

TEST_P(X86LegacySourceTest, VM86ModeBindingResumesTaskWithOriginalBudget) {
  taskState();
  O.Virtual8086.reset();
  put(0x8024, 0x20202, GuestScalarWidth::I32);
  const std::vector<SourceUnit> Units{{0x1000, 32, {0x9a, 0, 0, 0, 0, 0x30, 0}},
                                      {0x31b0, 16, {0x90}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  uint64_t Budget = 2;
  {
    X86LegacySourceExecution Context(S, *M, O);
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacyExecutionStatus::ModeUnavailable));
    EXPECT_EQ(Budget, 1u);
  }
  O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
  put(0x1000, 0xcc, GuestScalarWidth::I8);
  X86LegacySourceExecution Restored(S, *M, O);
  EXPECT_EQ(Run(&Restored, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x3001u);
  EXPECT_FALSE(S.PendingTask);
}

TEST_P(X86LegacySourceTest, VM86FarCallRetainsAliasedOperandAndAttemptBudget) {
  vm86State();
  S.Core.Segments[2] = S.Core.Segments[3] = x86VM86SegmentCache(0);
  S.Core.GPR[4] = 0xabcd1002;
  put(0x1000, 0x01000200, GuestScalarWidth::I32);
  put(0x11000, 0x21007, GuestScalarWidth::I32);
  const std::vector<SourceUnit> Units{{0x1100, 16, {0xff, 0x1e, 0, 0x10}},
                                      {0x1200, 16, {0x90}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  uint64_t Budget = 2;
  {
    X86LegacySourceExecution Context(S, *M, O);
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacyExecutionStatus::BackingFault));
    EXPECT_EQ(Budget, 1u);
  }
  put(0x1000, 0x55aa, GuestScalarWidth::I16);
  put(0x1100, 0xcc, GuestScalarWidth::I8);
  repairMpxBacking();
  X86LegacySourceExecution Restored(S, *M, O);
  EXPECT_EQ(Run(&Restored, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x201u);
  EXPECT_EQ(get(0x1000, GuestScalarWidth::I16), 0x55aau);
  EXPECT_FALSE(S.PendingFar);
}
