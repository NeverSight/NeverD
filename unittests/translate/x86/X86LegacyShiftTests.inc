//===- X86LegacyShiftTests.inc - Protected bit-motion contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent defined flags, count aliases, explicit undefined inputs and
/// transactional operand faults for shifts and rotates.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, ShiftOneRetainsEveryDefinedFlag) {
  struct Case {
    uint8_t Group, Value;
    uint32_t Flags;
  };
  for (Case C : {Case{0, 3, 0xa03},
                 {1, 0xc0, 0x203},
                 {2, 2, 0xa03},
                 {3, 0x40, 0xa03},
                 {4, 2, 0xa03},
                 {5, 0x40, 0xa03},
                 {7, 0xc0, 0x287}}) {
    S.Core.GPR[0] = 0x12345681;
    S.EFlags = 0x10202;
    O.ShiftUndefinedFlags = 0;
    ASSERT_TRUE(run({0xd0, uint8_t(0xc0 | C.Group << 3)}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x12345600u | C.Value);
    EXPECT_EQ(S.EFlags, C.Flags);
  }
}

TEST_F(X86LegacyExecutionTest, ShiftUndefinedImageCannotChangeDefinedBits) {
  for (uint32_t Image : {0u, kX86LegacyShiftUndefinedFlagsMask}) {
    O.ShiftUndefinedFlags = Image;
    S.Core.GPR[0] = 0x80000000;
    S.EFlags = 0x202;
    ASSERT_TRUE(run({0xd1, 0xe0}).InstructionCompleted); // SHL EAX, 1
    EXPECT_EQ(S.Core.GPR[0], 0u);
    EXPECT_EQ(S.EFlags, 0xa47u | (Image & 16));
    S.Core.GPR[0] = 0x80000000;
    ASSERT_TRUE(run({0xc1, 0xe8, 31}).InstructionCompleted); // SHR EAX, 31
    EXPECT_EQ(S.Core.GPR[0], 1u);
    EXPECT_EQ(S.EFlags, 0x202u | (Image & 0x810));
    S.Core.GPR[0] = 0x80000000;
    ASSERT_TRUE(run({0xc0, 0xe8, 8}).InstructionCompleted); // SHR AL, 8
    EXPECT_EQ(S.Core.GPR[0], 0x80000000u);
    EXPECT_EQ(S.EFlags, 0x246u | Image);
  }
}

TEST_F(X86LegacyExecutionTest, ShiftZeroMaskedCountsPreserveStateAndFlags) {
  O.ShiftUndefinedFlags = kX86LegacyShiftUndefinedFlagsMask;
  for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
    for (uint8_t Count : {0, 32, 64, 224}) {
      S.Core.GPR[0] = 0x81a5c37f;
      S.EFlags = 0x10ed7;
      ASSERT_TRUE(
          run({0xc1, uint8_t(0xc0 | Group << 3), Count}).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0x81a5c37fu);
      EXPECT_EQ(S.EFlags, 0xed7u);
    }
}

TEST_F(X86LegacyExecutionTest, RotateFullCyclesPreserveCarryOnlyWhenSpecified) {
  O.ShiftUndefinedFlags = 0;
  for (uint8_t Group : {0, 1}) {
    S.Core.GPR[0] = 0x81;
    S.EFlags = 0xed6;
    ASSERT_TRUE(
        run({0xc0, uint8_t(0xc0 | Group << 3), 8}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x81u);
    EXPECT_EQ(S.EFlags,
              0x6d7u); // Full ROL/ROR updates CF and binds undefined OF.
  }
  for (uint8_t Group : {2, 3})
    for (uint32_t Image : {0u, kX86LegacyShiftUndefinedFlagsMask}) {
      O.ShiftUndefinedFlags = Image;
      S.Core.GPR[0] = 0x81;
      S.EFlags = 0xed6;
      ASSERT_TRUE(
          run({0xc0, uint8_t(0xc0 | Group << 3), 9}).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0x81u);
      EXPECT_EQ(S.EFlags, 0x6d6u | (Image & 0x800));
    }
}

TEST_F(X86LegacyExecutionTest, ShiftCLIsCapturedBeforeDestinationPublication) {
  O.ShiftUndefinedFlags = 0;
  S.Core.GPR[1] = 0x80000021;
  S.EFlags = 0x202;
  ASSERT_TRUE(run({0xd3, 0xe9}).InstructionCompleted); // SHR ECX, CL
  EXPECT_EQ(S.Core.GPR[1], 0x40000010u);
  EXPECT_EQ(S.EFlags, 0xa03u);
  S.Core.GPR[1] = 0xabcd8101;
  S.EFlags = 0x202;
  ASSERT_TRUE(run({0xd2, 0xe5}).InstructionCompleted); // SHL CH, CL
  EXPECT_EQ(S.Core.GPR[1], 0xabcd0201u);
  EXPECT_EQ(S.EFlags, 0xa03u);
}

TEST_F(X86LegacyExecutionTest, ShiftCodeAndOperandWidthsRemainIndependent) {
  O.ShiftUndefinedFlags = 0;
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u}) {
      S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
      S.Core.GPR[0] = 0x80008080;
      std::vector<uint8_t> Bytes{0x67, 0x64};
      if (Width != 1 && (Width == 4) != Code32)
        Bytes.push_back(0x66);
      Bytes.insert(Bytes.end(), {uint8_t(Width == 1 ? 0xd0 : 0xd1), 0xf8});
      ASSERT_TRUE(run(Bytes).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], Width == 1   ? 0x800080c0u
                               : Width == 2 ? 0x8000c040u
                                            : 0xc0004040u);
    }
}

TEST_F(X86LegacyExecutionTest,
       ShiftZeroMemoryCountStillRequiresWritableOperand) {
  O.ShiftUndefinedFlags = 0;
  S.Core.GPR[3] = 0xc100;
  put(0xc100, 0x12345678);
  put(0x11030, 0xc005, GuestScalarWidth::I32);
  const auto R = run({0xc1, 0x23, 0});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xc100u);
  EXPECT_EQ(get(S.Core.GPR[4]), 7u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
  EXPECT_EQ(get(0xc100), 0x12345678u);
}

TEST_F(X86LegacyExecutionTest, ShiftCrossPageFaultPublishesNoPartialResult) {
  O.ShiftUndefinedFlags = 0;
  put(0x5804, 0xf000, GuestScalarWidth::I32);
  put(0xcfff, 0x87654321, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  S.Core.GPR[3] = 0xcfff;
  S.EFlags = 0xed7;
  const auto R = run({0xd1, 0x23});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xd000u);
  EXPECT_EQ(get(0xcfff), 0x87654321u);
  EXPECT_EQ(get(S.Core.GPR[4] + 12) & kX86LegacyArithmeticFlagsMask, 0x8d5u);
  EXPECT_EQ(get(S.Core.GPR[4]), 6u);
}

TEST_F(X86LegacyExecutionTest,
       ShiftBindingAndMalformedControlsRefuseBeforeFetch) {
  for (std::optional<uint32_t> Image :
       {std::optional<uint32_t>{}, std::optional<uint32_t>{0x10000}}) {
    O.ShiftUndefinedFlags = Image;
    auto R = executeX86LegacyInstruction(S, *M, {0xd1, 0xe0}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
  O.ShiftUndefinedFlags = 0;
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0xd1},
                                           {0xc1, 0xe0},
                                           {0xd1, 0xe0, 0},
                                           {0xf0, 0xd1, 0xe0},
                                           {0xf3, 0xd1, 0xe0},
                                           {0xd1, 0xf0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}
