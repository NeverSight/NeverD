//===- X86LegacyMpxSourceTests.inc - Compiled protected bounds flows ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Generated C retains bounds state, fault recovery and host-stop budgets.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacySourceTest, MpxInstructionsAndOriginalBranchEffects) {
  std::vector<SourceUnit> Units;
  uint32_t Address = 0x1000;
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xf3, 0x0f, 0x1b, 0x44, 8, 8},
        {0x66, 0x0f, 0x1a, 0xc8},
        {0x66, 0x0f, 0x1b, 0x0b},
        {0x66, 0x0f, 0x1a, 0x13},
        {0x0f, 0x1b, 0x14, 8},
        {0x0f, 0x1a, 0x1c, 8},
        {0xf3, 0x0f, 0x1a, 0xd8},
        {0xf2, 0x0f, 0x1a, 0xd8},
        {0xf2, 0x0f, 0x1b, 0xda},
        {0xf2, 0x74, 0},
        {0x74, 0}}) {
    Units.push_back({Address, 32, Bytes});
    Address += Bytes.size();
  }
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  bindMpx();
  S.Core.GPR[0] = 0x3004;
  S.Core.GPR[1] = 0x1234;
  S.Core.GPR[2] = 0;
  S.Core.GPR[3] = 0xc100;
  put(0x800c, 0xa001, GuestScalarWidth::I32);
  put(0xa01c, 0xdeadbeef, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  for (unsigned I = 0; I != Units.size(); ++I) {
    uint64_t Budget = 1;
    ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(S.Mpx->status(), 0x12345678u);
    if (I >= 5 && I != Units.size() - 1)
      EXPECT_EQ(*S.Mpx->bounds(3),
                (X86MpxBounds{{0x3004, UINT32_MAX}, {~0x4240u, UINT32_MAX}}));
  }
  for (unsigned I = 0; I != 4; ++I)
    EXPECT_EQ(*S.Mpx->bounds(I),
              (X86MpxBounds{{0, UINT64_MAX}, {0, UINT64_MAX}}));
  EXPECT_EQ(get(0xc100, GuestScalarWidth::I64), 0xffffbdbf00003004ULL);
  EXPECT_EQ(get(0xa010, GuestScalarWidth::I64), 0xffffbdbf00003004ULL);
  EXPECT_EQ(get(0xa018), 0x1234u);
  EXPECT_EQ(get(0xa01c), 0xdeadbeefu);
}

TEST_P(X86LegacySourceTest,
       MpxBoundsFaultHandlerReturnsAndRetriesOriginalCheck) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0xf3, 0x0f, 0x1a, 0xc0}},
                                   {0x2000, 32, {0xb8, 0, 1, 0, 0}},
                                   {0x2005, 32, {0xcf}}},
                                  Run));
  bindMpx();
  S.Core.GPR[0] = 0;
  const auto Before = S.Mpx->bounds(0);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 4;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 5);
  EXPECT_EQ(S.Mpx->status(), 1u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x1004u);
  EXPECT_EQ(S.Core.CPL, 3u);
  EXPECT_EQ(S.Core.GPR[0], 0x100u);
  EXPECT_EQ(S.Mpx->bounds(0), Before);
  EXPECT_EQ(S.Mpx->status(), 1u);
}

TEST_P(X86LegacySourceTest,
       MpxBackingResumeKeepsDirectoryAndDoesNotSpendTwice) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x1a, 4, 8}}}, Run));
  bindMpx(0x10001);
  S.Core.GPR[0] = 0x421000;
  S.Core.GPR[1] = 0x1234;
  put(0x11084, 0x21007, GuestScalarWidth::I32);
  {
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
    EXPECT_EQ(Budget, 0u);
    ASSERT_TRUE(S.PendingMpx);
    EXPECT_EQ(S.PendingMpx->Directory->Value, 0x21007u);
  }
  repairMpxBacking();
  put(0x21004, 0x2222222211111111ULL);
  put(0x2100c, 0x1234, GuestScalarWidth::I32);
  put(0x1000, 0xcc, GuestScalarWidth::I8);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 257u) << Context.lastError().str();
  EXPECT_EQ(Budget, 1u);
  EXPECT_FALSE(S.PendingMpx);
  EXPECT_EQ(*S.Mpx->bounds(0),
            (X86MpxBounds{{0x11111111, UINT32_MAX}, {0x22222222, UINT32_MAX}}));
  EXPECT_EQ(S.EIP, 0x1004u);
}
