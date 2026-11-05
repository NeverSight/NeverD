//===- X86LegacyBoundTests.inc - Restartable signed bounds checks --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent signed bounds, access order, fault frames and MPX publication.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest,
       BoundUsesSignedInclusiveLimitsAndPreservesIndex) {
  for (unsigned Width : {2u, 4u})
    for (uint32_t Index : {0xffff8000u, 0xfffffffbu, 0xfffffffcu, 0u, 4u, 5u,
                           0x7fffffffu, 0x80000000u}) {
      SetUp();
      S.Core.GPR[0] = Index;
      S.Core.GPR[3] = 0xc100;
      S.EFlags = 0x10ed7;
      const auto W = Width == 2 ? GuestScalarWidth::I16 : GuestScalarWidth::I32;
      put(0xc100, -4, W);
      put(0xc100 + Width, 4, W);
      const int32_t Signed = Width == 2 ? int16_t(Index) : int32_t(Index);
      const bool InRange = Signed >= -4 && Signed <= 4;
      const auto R = run(Width == 2 ? std::vector<uint8_t>{0x66, 0x62, 3}
                                    : std::vector<uint8_t>{0x62, 3});
      EXPECT_EQ(R.InstructionCompleted, InRange);
      EXPECT_EQ(S.Core.GPR[0], Index);
      if (InRange) {
        EXPECT_EQ(S.EFlags, 0xed7u);
      } else {
        EXPECT_EQ(R.DeliveredVector, 5);
        EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u); // #BR has no error code.
        EXPECT_EQ(get(S.Core.GPR[4] + 8), 0x10ed7u);
      }
    }
}

TEST_F(X86LegacyExecutionTest, BoundDoesNotReadUpperAfterLowerFailure) {
  for (unsigned Width : {2u, 4u})
    for (bool Limited : {false, true})
      for (int32_t Index : {-5, 0, 5}) {
        SetUp();
        S.Core.GPR[0] = Index;
        put(0x5804, 0xf000, GuestScalarWidth::I32);
        S.Core.GPR[3] = 0xd000 - Width;
        const auto W =
            Width == 2 ? GuestScalarWidth::I16 : GuestScalarWidth::I32;
        put(0xd000 - Width, -4, W);
        put(0xd000, 4, W);
        if (Limited)
          S.Core.Segments[3].Limit = 0xcfff;
        else
          put(0x11034, 0xd006, GuestScalarWidth::I32);
        auto R = run(Width == 2 ? std::vector<uint8_t>{0x66, 0x62, 3}
                                : std::vector<uint8_t>{0x62, 3});
        EXPECT_EQ(R.DeliveredVector, Index < -4 ? 5 : Limited ? 13 : 14);
        EXPECT_EQ(S.CR2, Index >= -4 && !Limited ? 0xd000u : 0x12345678u);
        EXPECT_EQ(get(0x11030) & 0x60, 0x20u);
        EXPECT_EQ(get(0x11034) & 0x60, 0u);
      }
}

TEST_F(X86LegacyExecutionTest, BoundUpperAddressWrapsBeforeSegmentChecks) {
  for (unsigned Width : {2u, 4u}) {
    SetUp();
    S.Core.GPR[0] = 0;
    S.Core.GPR[3] = 0x12350000 - Width;
    S.Core.Segments[3].Limit = 0xffff;
    const auto W = Width == 2 ? GuestScalarWidth::I16 : GuestScalarWidth::I32;
    put(0x10000 - Width, -4, W);
    put(0, 4, W);
    auto R = run(Width == 2 ? std::vector<uint8_t>{0x66, 0x67, 0x62, 7}
                            : std::vector<uint8_t>{0x67, 0x62, 7});
    EXPECT_TRUE(R.InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[3], 0x12350000u - Width);
  }
}

TEST_F(X86LegacyExecutionTest, BoundRestartsAfterOriginalBoundsAndPageRepairs) {
  for (bool Page : {false, true}) {
    SetUp();
    S.Core.GPR[0] = Page ? 0 : -5;
    put(0x5804, 0xf000, GuestScalarWidth::I32);
    S.Core.GPR[3] = 0xcffc;
    put(0xcffc, -4, GuestScalarWidth::I32);
    put(0xd000, 4, GuestScalarWidth::I32);
    if (Page)
      put(0x11034, 0xd006, GuestScalarWidth::I32);
    ASSERT_EQ(run({0x62, 3}).DeliveredVector, Page ? 14 : 5);
    if (Page) {
      ASSERT_TRUE(run({0xc7, 0x05, 0x34, 0x10, 1, 0, 7, 0xd0, 0, 0})
                      .InstructionCompleted);
      ASSERT_TRUE(run({0x83, 0xc4, 4}).InstructionCompleted);
    } else {
      ASSERT_TRUE(run({0xb8, 0, 0, 0, 0}).InstructionCompleted);
    }
    ASSERT_TRUE(run({0xcf}).InstructionCompleted);
    ASSERT_EQ(S.EIP, 0x1000u);
    EXPECT_TRUE(run({0x62, 3}).InstructionCompleted);
  }
}

TEST_F(X86LegacyExecutionTest, BoundMpxStatusBelongsToBRBeforeEventDelivery) {
  for (bool Enabled : {false, true})
    for (unsigned Case = 0; Case != 4; ++Case) {
      SetUp();
      auto Controls = M->controls();
      Controls.CR4 |= uint64_t{1} << 18;
      ASSERT_FALSE(M->bindControls(Controls));
      S.Mpx.emplace();
      ASSERT_TRUE(S.Mpx->configure({.Supported = true,
                                    .OSXSAVE = false,
                                    .XCR0 = X86MpxXcr0Mask,
                                    .PrivilegeLevel = 0,
                                    .UserConfig = Enabled ? 1u : 0u,
                                    .SupervisorConfig = Enabled ? 0u : 1u,
                                    .InTransaction = false}));
      S.Mpx->setStatus(0x12345678);
      ASSERT_TRUE(S.Mpx->setBounds(0, {{12, UINT64_MAX}, {34, UINT64_MAX}}));
      S.Core.GPR[0] = Case == 0 || Case == 3 ? -5 : 0;
      S.Core.GPR[3] = 0xc100;
      put(0xc100, -4, GuestScalarWidth::I32);
      put(0xc104, 4, GuestScalarWidth::I32);
      if (Case == 2)
        put(0x11030, 0xc006, GuestScalarWidth::I32);
      if (Case == 3)
        S.IDTRLimit = 0; // Failed #BR and #GP delivery reaches shutdown.
      auto R = run({0x62, 3});
      EXPECT_EQ(S.Mpx->status(),
                Enabled && (Case == 0 || Case == 3) ? 0u : 0x12345678u);
      EXPECT_EQ(S.Mpx->bounds(0)->Lower.Value, 12u);
      EXPECT_EQ(S.Mpx->bounds(0)->Upper.Value, 34u);
      if (Case == 3)
        EXPECT_EQ(R.Status, X86LegacyExecutionStatus::Shutdown);
    }
}

TEST_F(X86LegacyExecutionTest,
       BoundAdmissionRejectsUnknownStateAndMalformedInput) {
  for (const auto &B :
       std::vector<std::vector<uint8_t>>{{0x62},
                                         {0x62, 0xc0},
                                         {0xf0, 0x62, 3},
                                         {0x62, 3, 0},
                                         {0x66, 0x66, 0x62, 3}}) {
    auto R = executeX86LegacyInstruction(S, *M, B, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
  auto Controls = M->controls();
  Controls.CR4 |= uint64_t{1} << 18;
  ASSERT_FALSE(M->bindControls(Controls));
  auto R = executeX86LegacyInstruction(S, *M, {0x62, 3}, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("MPX"), std::string::npos);
  EXPECT_EQ(get(0x11004) & 0x20, 0u);
}

TEST_F(X86LegacyExecutionTest,
       BoundRejectsActiveTransactionsEvenWhenMpxIsDisabled) {
  for (bool OSXSAVE : {false, true})
    for (bool Enabled : {false, true}) {
      SetUp();
      auto Controls = M->controls();
      if (OSXSAVE)
        Controls.CR4 |= uint64_t{1} << 18;
      ASSERT_FALSE(M->bindControls(Controls));
      S.Mpx.emplace();
      ASSERT_TRUE(S.Mpx->configure({.Supported = true,
                                    .OSXSAVE = OSXSAVE,
                                    .XCR0 = X86MpxXcr0Mask,
                                    .PrivilegeLevel = 3,
                                    .UserConfig = Enabled ? 1u : 0u,
                                    .SupervisorConfig = 0,
                                    .InTransaction = true}));
      S.Mpx->setStatus(0x12345678);
      const auto Before = S;
      auto R = executeX86LegacyInstruction(S, *M, {0x62, 3}, O);
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find("transaction"),
                std::string::npos);
      EXPECT_EQ(get(0x11004) & 0x20, 0u);
      EXPECT_EQ(S.EIP, Before.EIP);
      EXPECT_EQ(S.EFlags, Before.EFlags);
      EXPECT_EQ(S.CR2, Before.CR2);
      EXPECT_EQ(S.Core.GPR, Before.Core.GPR);
      EXPECT_EQ(S.Mpx->status(), Before.Mpx->status());
    }
}
