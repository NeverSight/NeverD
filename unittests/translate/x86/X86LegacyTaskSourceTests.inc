//===- X86LegacyTaskSourceTests.inc - Compiled task dispatch -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Host-x64 Clang/NeverC drivers retain task switches and their continuations.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacySourceTest, TaskCallIretRedispatchesIncomingCodeMode) {
  taskState();
  put(0x7048, 0x008ffb000000ffffULL);
  put(0x804c, 0x4b, GuestScalarWidth::I16);
  const std::vector<SourceUnit> Units{{0x1000, 32, {0x9a, 0, 0, 0, 0, 0x30, 0}},
                                      {0x1007, 32, {0x90}},
                                      {0x3000, 16, {0xb8, 0x34, 0x12}},
                                      {0x3003, 16, {0xcf}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 4;
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x1008u);
  EXPECT_EQ(S.TR.Selector, 40);
  EXPECT_EQ(S.Core.GPR[0], 35u);
  EXPECT_EQ(get(0x8028), 0x1234u);
  EXPECT_EQ(get(0x8020), 0x3004u);
  EXPECT_FALSE(S.PendingTask);
}

TEST_P(X86LegacySourceTest,
       TaskBackingResumeSelectsOriginalWithoutExtraAttempt) {
  taskState();
  put(0x7038, taskDescriptor(0x9000, 0x3f, 0x82));
  put(0x8060, 0x38, GuestScalarWidth::I16);
  put(0x804c, 0x0f, GuestScalarWidth::I16);
  put(0x11024, 0x21007, GuestScalarWidth::I32);
  const std::vector<SourceUnit> Units{{0x1000, 32, {0x9a, 0, 0, 0, 0, 0x30, 0}},
                                      {0x3000, 32, {0x90}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  uint64_t Budget = 1;
  {
    X86LegacySourceExecution Context(S, *M, O);
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacyExecutionStatus::BackingFault));
    EXPECT_EQ(Budget, 0u);
    ASSERT_TRUE(S.PendingTask);
    EXPECT_EQ(S.EIP, 0x3000u);
  }
  repairMpxBacking();
  put(0x21008, 0x00cffb000000ffffULL);
  put(0x1000, 0xcc, GuestScalarWidth::I8);
  put(0x5820, 0xdeadbeef, GuestScalarWidth::I32);
  X86LegacySourceExecution Restored(S, *M, O);
  Budget = 1;
  EXPECT_EQ(Run(&Restored, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted))
      << Restored.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x3001u);
  EXPECT_EQ(get(0x5820), 0xdeadbeefu);
  EXPECT_FALSE(S.PendingTask);
}

TEST_P(X86LegacySourceTest, TaskIncomingFaultDispatchesHandlerWithNewFrame) {
  taskState();
  put(0x7038, 0x00cf73000000ffffULL);
  put(0x8054, 0x3b, GuestScalarWidth::I16);
  const std::vector<SourceUnit> Units{
      {0x1000, 32, {0x9a, 0, 0, 0, 0, 0x30, 0}},
      {0x2000, 32, {0xb8, 0x78, 0x56, 0x34, 0x12}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacyExecutionStatus::EventDelivered));
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x3000u);
  EXPECT_FALSE(S.PendingTask);
  Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
}

TEST_P(X86LegacySourceTest, TaskVM86DestinationExecutesWithParagraphBase) {
  taskState();
  put(0x8024, 0x20202, GuestScalarWidth::I32);
  const std::vector<SourceUnit> Units{{0x1000, 32, {0x9a, 0, 0, 0, 0, 0x30, 0}},
                                      {0x31b0, 16, {0xb8, 0x34, 0x12}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  install(Units);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 2;
  EXPECT_EQ(Run(&Context, &Budget),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.TR.Selector, 48);
  EXPECT_EQ(S.EIP, 0x3003u);
  EXPECT_EQ(S.Core.GPR[0], 0x1234u);
  EXPECT_FALSE(S.PendingTask);
}
