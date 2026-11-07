//===- X86LegacyDivisionTests.inc - Protected divide contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full dividends, explicit undefined flags, ordered reads and restartable #DE.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, DivisionRequiresFlagImageBeforeFetch) {
  for (const auto Flags :
       {std::optional<uint32_t>{}, std::optional<uint32_t>{0x200},
        std::optional<uint32_t>{0x10000}}) {
    O.DivisionArithmeticFlags = Flags;
    auto R = executeX86LegacyInstruction(S, *M, {0xf7, 0xf1}, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("arithmetic-flags image"),
              std::string::npos);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, DivisionCapturesAliasedByteOperands) {
  O.DivisionArithmeticFlags = 0x804;
  S.Core.GPR[0] = 0xabcd0102;
  S.Core.GPR[2] = 0x76543210;
  ASSERT_TRUE(run({0xf6, 0xf0}).InstructionCompleted); // DIV AL.
  EXPECT_EQ(S.Core.GPR[0], 0xabcd0081u);
  EXPECT_EQ(S.Core.GPR[2], 0x76543210u);
  EXPECT_EQ(S.EFlags, 0xa06u);
  S.Core.GPR[0] = 0xabcdfff6;
  ASSERT_TRUE(run({0xf6, 0xfc}).InstructionCompleted); // IDIV AH.
  EXPECT_EQ(S.Core.GPR[0], 0xabcd000au);
  EXPECT_EQ(S.Core.GPR[2], 0x76543210u);
}

TEST_F(X86LegacyExecutionTest,
       DivisionUsesBothDividendHalvesAndSignedRemainder) {
  O.DivisionArithmeticFlags = 0;
  S.Core.GPR[0] = 0xabcd0000;
  S.Core.GPR[2] = 0x76540001;
  S.Core.GPR[1] = 3;
  ASSERT_TRUE(run({0x66, 0xf7, 0xf1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd5555u);
  EXPECT_EQ(S.Core.GPR[2], 0x76540001u);
  S.Core.GPR[0] = 0;
  S.Core.GPR[2] = 1;
  ASSERT_TRUE(run({0xf7, 0xf1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x55555555u);
  EXPECT_EQ(S.Core.GPR[2], 1u);
  S.Core.GPR[0] = uint32_t(-10);
  S.Core.GPR[2] = UINT32_MAX;
  ASSERT_TRUE(run({0xf7, 0xf9}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], uint32_t(-3));
  EXPECT_EQ(S.Core.GPR[2], UINT32_MAX);
  S.Core.GPR[0] = 10;
  S.Core.GPR[2] = 0;
  S.Core.GPR[1] = uint32_t(-3);
  ASSERT_TRUE(run({0xf7, 0xf9}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], uint32_t(-3));
  EXPECT_EQ(S.Core.GPR[2], 1u);
  EXPECT_EQ(S.EFlags, 0x202u);
}

TEST_F(X86LegacyExecutionTest, DivideErrorsPublishOnlyTheRestartFrame) {
  struct Case {
    std::vector<uint8_t> Bytes;
    uint32_t Low, High, Divisor;
  };
  for (const Case &C :
       std::vector<Case>{{{0xf7, 0xf1}, 10, 0, 0},
                         {{0xf6, 0xf1}, 0x100, 0, 1},
                         {{0xf6, 0xf9}, 0xff80, 0, 0xff},
                         {{0x66, 0xf7, 0xf1}, 0, 1, 1},
                         {{0xf7, 0xf1}, 0, 1, 1},
                         {{0xf7, 0xf9}, 0, 0x80000000, UINT32_MAX}}) {
    SetUp();
    O.DivisionArithmeticFlags = 0;
    S.EFlags = 0x10ad7;
    S.Core.GPR[0] = C.Low;
    S.Core.GPR[2] = C.High;
    S.Core.GPR[1] = C.Divisor;
    const auto R = run(C.Bytes);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_EQ(R.DeliveredVector, 0);
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], C.Low);
    EXPECT_EQ(S.Core.GPR[2], C.High);
    EXPECT_EQ(S.Core.GPR[1], C.Divisor);
    EXPECT_EQ(S.CR2, 0x12345678u);
    EXPECT_EQ(S.Core.GPR[4], 0xdfecu);
    const uint32_t Frame[]{0x1000, 27, 0x10ad7, 0xd000, 35};
    for (unsigned I = 0; I != 5; ++I)
      EXPECT_EQ(get(S.Core.GPR[4] + I * 4), Frame[I]);
  }
}

TEST_F(X86LegacyExecutionTest, DivisionReadsReadonlyMemoryWithoutDirtyingIt) {
  O.DivisionArithmeticFlags = 0;
  S.Core.GPR[0] = 21;
  S.Core.GPR[2] = 0;
  S.Core.GPR[3] = 0xc100;
  put(0xc100, 7, GuestScalarWidth::I32);
  put(0x11030, 0xc005, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0xf7, 0x33}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 3u);
  EXPECT_EQ(S.Core.GPR[2], 0u);
  EXPECT_EQ(get(0xc100), 7u);
  EXPECT_EQ(get(0x11030) & 0x60, 0x20u);
}

TEST_F(X86LegacyExecutionTest, DivisionOperandFaultPrecedesDivideError) {
  O.DivisionArithmeticFlags = 0;
  S.Core.GPR[0] = 0x76543210;
  S.Core.GPR[2] = 0x12345678;
  S.Core.GPR[3] = 0xc100;
  put(0xc100, 0, GuestScalarWidth::I32);
  put(0x11030, 0xc006, GuestScalarWidth::I32);
  const auto R = run({0xf7, 0x33});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xc100u);
  EXPECT_EQ(S.Core.GPR[0], 0x76543210u);
  EXPECT_EQ(S.Core.GPR[2], 0x12345678u);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u); // User read, not a prepared write.
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
}

TEST_F(X86LegacyExecutionTest, DivideHandlerChangesOperandAndReturnsToRetry) {
  O.DivisionArithmeticFlags = kX86LegacyArithmeticFlagsMask;
  S.Core.GPR[0] = 10;
  S.Core.GPR[2] = S.Core.GPR[1] = 0;
  ASSERT_EQ(run({0xf7, 0xf9}).DeliveredVector, 0);
  ASSERT_TRUE(run({0xb9, 3, 0, 0, 0}).InstructionCompleted);
  ASSERT_TRUE(run({0xcf}).InstructionCompleted);
  EXPECT_EQ(S.EIP, 0x1000u);
  ASSERT_TRUE(run({0xf7, 0xf9}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 3u);
  EXPECT_EQ(S.Core.GPR[2], 1u);
  EXPECT_EQ(S.EFlags, 0xad7u);
}
