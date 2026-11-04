//===- X86LegacyMultiplyTests.inc - Protected multiply contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full/truncated products, explicit undefined flags and ordered operand reads.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, MultiplyRequiresAFlagProfileBeforeFetch) {
  for (auto Profile : {std::optional<X86LegacyMultiplyProfile>{},
                       std::optional{X86LegacyMultiplyProfile(255)}}) {
    O.Multiply = Profile;
    auto R = executeX86LegacyInstruction(S, *M, {0xf7, 0xe1}, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("bound flags profile"),
              std::string::npos);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, MultiplyWritesTheFullUnsignedProduct) {
  O.Multiply = X86LegacyMultiplyProfile::ProductSignParity;
  S.Core.GPR[0] = 0xabcd00ff;
  S.Core.GPR[1] = 2;
  S.Core.GPR[2] = 0x76543210;
  ASSERT_TRUE(run({0xf6, 0xe1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd01feu);
  EXPECT_EQ(S.Core.GPR[2], 0x76543210u);
  EXPECT_EQ(S.EFlags, 0xa83u);
  S.Core.GPR[0] = 0xabcdffff;
  ASSERT_TRUE(run({0x66, 0xf7, 0xe1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcdfffeu);
  EXPECT_EQ(S.Core.GPR[2], 0x76540001u);
  S.Core.GPR[0] = UINT32_MAX;
  ASSERT_TRUE(run({0xf7, 0xe1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xfffffffeu);
  EXPECT_EQ(S.Core.GPR[2], 1u);
}

TEST_F(X86LegacyExecutionTest, SignedMultiplyChecksRepresentability) {
  O.Multiply = X86LegacyMultiplyProfile::ProductSignParity;
  S.Core.GPR[0] = 0xabcd0080;
  S.Core.GPR[1] = UINT32_MAX;
  ASSERT_TRUE(run({0xf6, 0xe9}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd0080u);
  EXPECT_EQ(S.EFlags, 0xa83u); // +128 does not fit a signed byte.
  S.Core.GPR[0] = 0xabcd0080;
  S.Core.GPR[1] = 1;
  ASSERT_TRUE(run({0xf6, 0xe9}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcdff80u);
  EXPECT_EQ(S.EFlags, 0x282u);
  S.Core.GPR[0] = S.Core.GPR[1] = 0x80000000;
  ASSERT_TRUE(run({0xf7, 0xe9}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0u);
  EXPECT_EQ(S.Core.GPR[2], 0x40000000u);
  EXPECT_EQ(S.EFlags, 0xa07u);
}

TEST_F(X86LegacyExecutionTest, MultiplyReadsHighByteAliasesBeforeWritingAX) {
  O.Multiply = X86LegacyMultiplyProfile::ProductSignParity;
  S.Core.GPR[0] = 0xabcd0302;
  S.Core.GPR[2] = 0x76543210;
  ASSERT_TRUE(run({0xf6, 0xe4}).InstructionCompleted); // MUL AH.
  EXPECT_EQ(S.Core.GPR[0], 0xabcd0006u);
  EXPECT_EQ(S.Core.GPR[2], 0x76543210u);
  S.Core.GPR[0] = 0xabcdfffe;
  ASSERT_TRUE(run({0xf6, 0xec}).InstructionCompleted); // IMUL AH.
  EXPECT_EQ(S.Core.GPR[0], 0xabcd0002u);
  EXPECT_EQ(S.EFlags & 0x801, 0u);
}

TEST_F(X86LegacyExecutionTest, TruncatedMultiplyKeepsTheOtherProductRegisters) {
  O.Multiply = X86LegacyMultiplyProfile::ProductSignParity;
  S.Core.GPR[0] = 0x12345678;
  S.Core.GPR[1] = 2;
  S.Core.GPR[2] = 0xabcd8000;
  ASSERT_TRUE(run({0x66, 0x0f, 0xaf, 0xd1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.Core.GPR[2], 0xabcd0000u);
  EXPECT_EQ(S.EFlags, 0xa07u);
  S.Core.GPR[1] = 3;
  ASSERT_TRUE(run({0x6b, 0xc1, 0xff}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], uint32_t(-3));
  EXPECT_EQ(S.Core.GPR[2], 0xabcd0000u);
  EXPECT_EQ(S.EFlags & 0x801, 0u);
  ASSERT_TRUE(run({0x69, 0xd1, 0, 0, 0, 0x80}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], uint32_t(-3));
  EXPECT_EQ(S.Core.GPR[2], 0x80000000u);
  EXPECT_EQ(S.EFlags, 0xa87u);
}

TEST_F(X86LegacyExecutionTest, MultiplyZeroProfilesRemainExplicit) {
  for (auto Profile : {X86LegacyMultiplyProfile::ProductSignParity,
                       X86LegacyMultiplyProfile::ProductSignParityZero}) {
    O.Multiply = Profile;
    S.Core.GPR[0] = 0;
    S.Core.GPR[1] = 7;
    S.EFlags = 0x10ad7;
    ASSERT_TRUE(run({0xf7, 0xe1}).InstructionCompleted);
    EXPECT_EQ(S.EFlags, Profile == X86LegacyMultiplyProfile::ProductSignParity
                            ? 0x206u
                            : 0x246u);
  }
}

TEST_F(X86LegacyExecutionTest, MultiplyReadsReadonlyMemoryAndRestartsOnFault) {
  O.Multiply = X86LegacyMultiplyProfile::ProductSignParity;
  S.Core.GPR[0] = 3;
  S.Core.GPR[3] = 0xc100;
  put(0xc100, 7, GuestScalarWidth::I32);
  put(0x11030, 0xc005, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0xf7, 0x23}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 21u);
  EXPECT_EQ(S.Core.GPR[2], 0u);
  EXPECT_EQ(get(0xc100), 7u);
  EXPECT_EQ(get(0x11030) & 0x60, 0x20u);
  const uint32_t Restart = S.EIP;
  S.Core.GPR[0] = 0x12345678;
  S.Core.GPR[2] = 0x76543210;
  put(0x11030, 0xc004, GuestScalarWidth::I32);
  EXPECT_EQ(run({0xf7, 0x2b}).DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.Core.GPR[2], 0x76543210u);
  EXPECT_EQ(S.CR2, 0xc100u);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u); // User read, no speculative write.
  EXPECT_EQ(get(S.Core.GPR[4] + 4), Restart);
}

TEST_F(X86LegacyExecutionTest, MultiplyRejectsIncompleteImmediateAndPrefixes) {
  O.Multiply = X86LegacyMultiplyProfile::ProductSignParity;
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x69, 0xc1, 1},
                                         {0x6b, 0xc1},
                                         {0x0f, 0xaf},
                                         {0xf0, 0xf7, 0xe1},
                                         {0xf3, 0x0f, 0xaf, 0xc1}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
  }
}
