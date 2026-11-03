//===- X86LegacyFastSystemTests.inc - Protected SEP contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit MSRs, coupled caches, restart state and subsequent target fetch.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, FastSystemRequiresBindingBeforeFetch) {
  auto R = executeX86LegacyInstruction(S, *M, {0x0f, 0x34}, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("Intel SEP"), std::string::npos);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(get(0x11004), 0x1007u);
}

TEST_F(X86LegacyExecutionTest,
       SysenterPublishesCachesWithoutTableOrStackReads) {
  O.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
  S.Sysenter = {0xfff8, 0xdeadbeef, 0x12345678};
  S.Core.Segments[1].Base = 0x8000;
  S.Core.Segments[1].Limit = 0x1001;
  S.Core.Segments[1].Attributes &= ~0x8000;
  S.EFlags = 0x3d7ed7;
  S.NMIBlocked = S.SSBlocked = S.STIBlocked = true;
  const auto Before = S;
  put(0x9000, 0x340f, GuestScalarWidth::I16);
  for (unsigned Page : {5u, 6u, 7u})
    put(0x11000 + Page * 4, Page * 4096 | 6, GuestScalarWidth::I32);
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x0f, 0x34}, O));
  ASSERT_TRUE(R.InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x12345678u);
  EXPECT_EQ(S.Core.GPR[4], 0xdeadbeefu);
  EXPECT_EQ(S.Core.CPL, 0);
  EXPECT_EQ(S.Core.Segments[1].Selector, 0xfff8);
  EXPECT_EQ(S.Core.Segments[2].Selector, 0);
  for (unsigned I : {1u, 2u}) {
    EXPECT_EQ(S.Core.Segments[I].Base, 0u);
    EXPECT_EQ(S.Core.Segments[I].Limit, UINT32_MAX);
    EXPECT_EQ(S.Core.Segments[I].Attributes, I == 1 ? 0xc09b : 0xc093);
    EXPECT_FALSE(S.Core.Segments[I].Unusable);
  }
  for (unsigned I : {0u, 1u, 2u, 3u, 5u, 6u, 7u})
    EXPECT_EQ(S.Core.GPR[I], Before.Core.GPR[I]);
  for (unsigned I : {0u, 3u, 4u, 5u}) {
    EXPECT_EQ(S.Core.Segments[I].Selector, Before.Core.Segments[I].Selector);
    EXPECT_EQ(S.Core.Segments[I].Attributes,
              Before.Core.Segments[I].Attributes);
  }
  EXPECT_EQ(S.EFlags, Before.EFlags & ~0x30200u);
  EXPECT_EQ(S.CR2, Before.CR2);
  EXPECT_TRUE(S.NMIBlocked);
  EXPECT_FALSE(S.SSBlocked);
  EXPECT_FALSE(S.STIBlocked);
}

TEST_F(X86LegacyExecutionTest, SysexitUsesFullECXEDXAndKeepsOtherFlags) {
  O.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
  S.Core.CPL = 0;
  S.Core.Segments[1] = {0, UINT32_MAX, 8, 0x809b, false};
  S.Core.Segments[2] = {0, UINT32_MAX, 16, 0x8093, false};
  S.Sysenter = {0xfff0, 0x11111111, 0x22222222};
  S.Core.GPR[1] = 0xffffffff;
  S.Core.GPR[2] = 0xfeedc0de;
  S.EFlags = 0x3d7ed7;
  S.NMIBlocked = true;
  ASSERT_TRUE(run({0x0f, 0x35}).InstructionCompleted);
  EXPECT_EQ(S.EIP, 0xfeedc0deu);
  EXPECT_EQ(S.Core.GPR[4], 0xffffffffu);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(S.Core.Segments[1].Selector, 3);
  EXPECT_EQ(S.Core.Segments[1].Attributes, 0xc0fb);
  EXPECT_EQ(S.Core.Segments[2].Selector, 11);
  EXPECT_EQ(S.Core.Segments[2].Attributes, 0xc0f3);
  EXPECT_EQ(S.EFlags, 0x3c7ed7u);
  EXPECT_TRUE(S.NMIBlocked);
  EXPECT_EQ(S.Sysenter.ESP, 0x11111111u);
  EXPECT_EQ(S.Sysenter.EIP, 0x22222222u);
}

TEST_F(X86LegacyExecutionTest, FastSystemGeneralProtectionKeepsRestartInputs) {
  for (bool Enter : {false, true})
    for (unsigned CPL : {0u, 3u})
      for (uint32_t CS : {0u, 3u, 8u}) {
        if (CS == 8 && (Enter || CPL == 0))
          continue;
        SetUp();
        O.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
        if (CPL == 0) {
          S.Core.CPL = 0;
          S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
          S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
        }
        S.Sysenter = {CS, 0xf000, 0x4000};
        const auto Before = S;
        auto R = run({0x0f, uint8_t(Enter ? 0x34 : 0x35)});
        ASSERT_EQ(R.DeliveredVector, 13);
        EXPECT_FALSE(R.InstructionCompleted);
        EXPECT_EQ(get(S.Core.GPR[4]), 0u);
        EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
        EXPECT_EQ(get(S.Core.GPR[4] + 8), Before.Core.Segments[1].Selector);
        EXPECT_EQ(get(S.Core.GPR[4] + 12), Before.EFlags);
        for (unsigned I : {0u, 1u, 2u, 3u, 5u, 6u, 7u})
          EXPECT_EQ(S.Core.GPR[I], Before.Core.GPR[I]);
        EXPECT_EQ(S.Sysenter.CS, CS);
        EXPECT_EQ(S.Sysenter.ESP, 0xf000u);
        EXPECT_EQ(S.Sysenter.EIP, 0x4000u);
      }
}

TEST_F(X86LegacyExecutionTest,
       LockedFastSystemDeliversUDBeforePrivilegeChecks) {
  for (bool Enter : {false, true}) {
    SetUp();
    O.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
    auto R = run({0xf0, 0x0f, uint8_t(Enter ? 0x34 : 0x35)});
    ASSERT_EQ(R.DeliveredVector, 6);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 12), 0xd000u);
  }
}

TEST_F(X86LegacyExecutionTest, SysenterTargetFaultBelongsToTheFollowingStep) {
  O.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
  S.Sysenter = {8, 0xd000, 0x4000};
  put(0x11010, 0x4006, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x0f, 0x34}).InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x4000u);
  EXPECT_EQ(S.Core.CPL, 0);
  const auto R = run({0x90});
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.CR2, 0x4000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x4000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 8), 8u);
}

TEST_F(X86LegacyExecutionTest, FastSystemPrefixesKeepThirtyTwoBitTargets) {
  for (const std::vector<uint8_t> Prefix : {std::vector<uint8_t>{0x66},
                                            {0x67},
                                            {0xf2},
                                            {0xf3},
                                            {0x36},
                                            {0x66, 0xf3, 0x67, 0x36}}) {
    SetUp();
    O.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
    S.Sysenter = {8, 0x12345678, 0x87654321};
    auto Bytes = Prefix;
    Bytes.insert(Bytes.end(), {0x0f, 0x34});
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(S.EIP, 0x87654321u);
    EXPECT_EQ(S.Core.GPR[4], 0x12345678u);
  }
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x66, 0x66, 0x0f, 0x34},
        {0xf2, 0xf3, 0x0f, 0x34},
        {0x0f, 0x34, 0},
        {0xf3, 0x8e, 0xd0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}
