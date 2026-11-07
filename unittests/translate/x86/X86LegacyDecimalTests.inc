//===- X86LegacyDecimalTests.inc - Protected decimal contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit undefined outputs, nondecimal bases and restartable AAM faults.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, DecimalFlagsAreValidatedBeforeFetch) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x27}, {0x2f}, {0x37}, {0x3f}, {0xd4, 10}, {0xd5, 0}}) {
    for (const auto Flags :
         {std::optional<uint32_t>{}, std::optional<uint32_t>{0x200},
          std::optional<uint32_t>{0x8d5}}) {
      O.DecimalUndefinedFlags = Flags;
      auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find("undefined-flags image"),
                std::string::npos);
      EXPECT_EQ(S.EIP, 0x1000u);
      EXPECT_EQ(get(0x11004) & 0x20, 0u);
    }
  }
}

TEST_F(X86LegacyExecutionTest, DecimalAsciiCarryAndBorrowSpanBothBytes) {
  struct Case {
    uint8_t Opcode;
    uint16_t Before, After;
    uint32_t Input, Defined;
  };
  for (const auto C :
       {Case{0x37, 0x12ff, 0x1405, 0, 0x11},
        Case{0x37, 0xffff, 0x0105, 0, 0x11}, Case{0x37, 0x1259, 0x1209, 1, 0},
        Case{0x37, 0x1200, 0x1306, 0x10, 0x11},
        Case{0x3f, 0x1200, 0x100a, 0x10, 0x11},
        Case{0x3f, 0x0000, 0xfe0a, 0x10, 0x11},
        Case{0x3f, 0x120a, 0x1104, 0, 0x11},
        Case{0x3f, 0x1209, 0x1209, 1, 0}}) {
    for (unsigned Choice = 0; Choice != 16; ++Choice) {
      O.DecimalUndefinedFlags =
          ((Choice & 1) ? 0x800 : 0) | ((Choice & 2) ? 0x80 : 0) |
          ((Choice & 4) ? 0x40 : 0) | ((Choice & 8) ? 4 : 0);
      S.Core.GPR[0] = 0xabcd0000 | C.Before;
      S.EFlags = 0x602 | C.Input;
      ASSERT_TRUE(run({0x66, 0x67, 0x65, C.Opcode}).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0xabcd0000u | C.After);
      EXPECT_EQ(S.EFlags, 0x602u | C.Defined | *O.DecimalUndefinedFlags);
    }
  }
}

TEST_F(X86LegacyExecutionTest, DecimalPackedAdjustmentRetainsEarlyBorrow) {
  struct Case {
    uint8_t Opcode, Before, After;
    uint32_t Input, Defined;
  };
  for (const auto C :
       {Case{0x27, 0x99, 0x99, 0, 0x84}, Case{0x27, 0x9a, 0, 0, 0x55},
        Case{0x27, 0xff, 0x65, 0, 0x15}, Case{0x27, 0x05, 0x0b, 0x10, 0x10},
        Case{0x2f, 0, 0xfa, 0x10, 0x95}, Case{0x2f, 0x05, 0xff, 0x10, 0x95},
        Case{0x2f, 0x9a, 0x34, 0, 0x11}, Case{0x2f, 0, 0xa0, 1, 0x85}}) {
    for (uint32_t Undefined : {0u, 0x800u}) {
      O.DecimalUndefinedFlags = Undefined;
      S.Core.GPR[0] = 0xabcd1200 | C.Before;
      S.EFlags = 0x602 | C.Input;
      ASSERT_TRUE(run({C.Opcode}).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0xabcd1200u | C.After);
      EXPECT_EQ(S.EFlags, 0x602u | C.Defined | Undefined);
    }
  }
}

TEST_F(X86LegacyExecutionTest, DecimalEveryBaseKeepsFullImmediateAndAXScope) {
  for (uint8_t Opcode : {0xd4, 0xd5})
    for (unsigned Base = Opcode == 0xd4 ? 1 : 0; Base != 256; ++Base)
      for (uint16_t AX : {0u, 1u, 0x00ffu, 0x127fu, 0x8000u, 0xffffu})
        for (uint32_t Undefined : {0u, 0x811u}) {
          S.EIP = 0x1000;
          O.DecimalUndefinedFlags = Undefined;
          S.Core.GPR[0] = 0xabcd0000 | AX;
          S.EFlags = 0x602;
          ASSERT_TRUE(run({Opcode, uint8_t(Base)}).InstructionCompleted);
          const unsigned AL = AX & 255;
          const uint16_t Expected = Opcode == 0xd4
                                        ? (AL / Base) * 256 + AL % Base
                                        : (AL + (AX >> 8) * Base) % 256;
          EXPECT_EQ(S.Core.GPR[0], 0xabcd0000u | Expected);
          EXPECT_EQ(S.EFlags & 0x811, Undefined);
          EXPECT_EQ(S.EFlags & 0x600, 0x600u);
        }
}

TEST_F(X86LegacyExecutionTest, DecimalAAMZeroPublishesOnlyRestartFrame) {
  O.DecimalUndefinedFlags = 0;
  S.Core.GPR[0] = 0xabcd1295;
  S.EFlags = 0x10ed7;
  const auto R = run({0x66, 0xd4, 0});
  ASSERT_EQ(R.DeliveredVector, 0);
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd1295u);
  EXPECT_EQ(S.CR2, 0x12345678u);
  const uint32_t Frame[]{0x1000, 27, 0x10ed7, 0xd000, 35};
  for (unsigned I = 0; I != 5; ++I)
    EXPECT_EQ(get(S.Core.GPR[4] + I * 4), Frame[I]);
  ASSERT_TRUE(run({0xc6, 0x05, 2, 0x10, 0, 0, 10}).InstructionCompleted);
  ASSERT_TRUE(run({0xcf}).InstructionCompleted);
  ASSERT_EQ(S.EIP, 0x1000u);
  auto Retry =
      llvm::cantFail(executeX86LegacyInstruction(S, *M, {0x66, 0xd4, 10}, O));
  ASSERT_TRUE(Retry.InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd0e09u);
}

TEST_F(X86LegacyExecutionTest, DecimalAAMFetchPrecedesDivideError) {
  O.DecimalUndefinedFlags = 0x811;
  S.EIP = 0x1fff;
  put(0x1fff, 0xd4, GuestScalarWidth::I8);
  put(0x11008, 0x2006, GuestScalarWidth::I32);
  auto R = llvm::cantFail(executeX86LegacyInstruction(S, *M, {0xd4, 0}, O));
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x2000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1fffu);
}

TEST_F(X86LegacyExecutionTest, DecimalMalformedFormsRefuseWithoutEffects) {
  O.DecimalUndefinedFlags = 0;
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xf0, 0x27},
                                         {0xf3, 0x37},
                                         {0xf2, 0xd5, 10},
                                         {0xd4},
                                         {0xd5},
                                         {0x27, 0},
                                         {0xd4, 10, 0},
                                         {0x66, 0x66, 0x2f}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.EFlags, 0x10202u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}
