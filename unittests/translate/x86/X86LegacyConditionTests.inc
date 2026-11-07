//===- X86LegacyConditionTests.inc - Protected conditional scalars -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Conditional data accesses and independent loop count/target widths.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, SetConditionWritesOnlyTheSelectedByte) {
  for (bool Zero : {false, true}) {
    S.Core.GPR[0] = 0x12345678;
    S.EFlags = 0x10e97 | (Zero ? 64 : 0);
    // SETZ ignores the ModRM reg field, including a nonzero encoding.
    ASSERT_TRUE(run({0x66, 0x67, 0x64, 0x0f, 0x94, 0xfc}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x12340078u | (Zero ? 0x100 : 0));
    EXPECT_EQ(S.EFlags, 0xe97u | (Zero ? 64 : 0));
  }
}

TEST_F(X86LegacyExecutionTest,
       ConditionalMoveReadsEvenWhenTheConditionIsFalse) {
  S.Core.GPR[0] = 0x12345678;
  S.Core.GPR[3] = 0xc000;
  S.EFlags = 0x10202; // CMOVZ is false.
  put(0x11030, 0xc006, GuestScalarWidth::I32);
  const auto R = run({0x0f, 0x44, 0x03});
  ASSERT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.CR2, 0xc000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 12), 0x10202u);
}

TEST_F(X86LegacyExecutionTest, ConditionalMoveUsesReadonlySourceAndWordWrites) {
  for (bool Zero : {false, true}) {
    S.Core.GPR[0] = 0xabcd5678;
    S.Core.GPR[3] = 0xc000;
    S.EFlags = 0x10e97 | (Zero ? 64 : 0);
    put(0xc000, 0x87654321, GuestScalarWidth::I32);
    put(0x11030, 0xc005, GuestScalarWidth::I32);
    ASSERT_TRUE(run({0x66, 0x0f, 0x44, 0x03}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], Zero ? 0xabcd4321u : 0xabcd5678u);
    EXPECT_EQ(S.EFlags, 0xe97u | (Zero ? 64 : 0));
    EXPECT_EQ(get(0x11030) & 0x60, 0x20u);
    EXPECT_EQ(get(0xc000), 0x87654321u);
  }
}

TEST_F(X86LegacyExecutionTest, SetConditionOwnsAnExactByteStore) {
  S.Core.GPR[3] = 0xcfff;
  S.EFlags = 0x10202;
  put(0xcffc, 0xa5b6c7d8, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0x0f, 0x94, 0x03}).InstructionCompleted);
  EXPECT_EQ(get(0xcffc), 0x00b6c7d8u);
  EXPECT_EQ(get(0x11030) & 0x60, 0x60u);
  EXPECT_EQ(get(0x11034) & 0x60, 0u);
  EXPECT_EQ(S.EFlags, 0x202u);
}

TEST_F(X86LegacyExecutionTest, SetConditionCannotWriteReadonlyMemory) {
  S.Core.GPR[3] = 0xc000;
  S.EFlags = 0x10242;
  put(0xc000, 0x12345678, GuestScalarWidth::I32);
  put(0x11030, 0xc005, GuestScalarWidth::I32);
  ASSERT_EQ(run({0x0f, 0x94, 0x03}).DeliveredVector, 14);
  EXPECT_EQ(get(S.Core.GPR[4]), 7u);
  EXPECT_EQ(get(0xc000), 0x12345678u);
}

TEST_F(X86LegacyExecutionTest, LoopCounterAndTargetWidthsAreIndependent) {
  for (bool Operand32 : {false, true})
    for (bool Address32 : {false, true}) {
      S.EIP = 0x1ff00;
      S.EFlags = 0x10ed7;
      S.Core.GPR[1] = 0xabcd0000;
      std::vector<uint8_t> Bytes;
      if (!Operand32)
        Bytes.push_back(0x66);
      if (!Address32)
        Bytes.push_back(0x67);
      Bytes.insert(Bytes.end(), {0xe2, 0x20});
      ASSERT_TRUE(run(Bytes).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[1], Address32 ? 0xabccffffu : 0xabcdffffu);
      EXPECT_EQ(S.EIP, (Operand32 ? 0x1ff00u : 0xff00u) + Bytes.size() + 0x20);
      EXPECT_EQ(S.EFlags, 0xed7u);
    }
}

TEST_F(X86LegacyExecutionTest, LoopConditionsPreserveFlagsAndConsumeTheCount) {
  for (uint8_t Opcode : {0xe0, 0xe1, 0xe2})
    for (bool Zero : {false, true})
      for (uint32_t Count : {1u, 2u}) {
        S.EIP = 0x1000;
        S.EFlags = 0x10e97 | (Zero ? 64 : 0);
        S.Core.GPR[1] = Count;
        ASSERT_TRUE(run({Opcode, 0x20}).InstructionCompleted);
        const bool Take =
            Count == 2 && (Opcode == 0xe2 || (Opcode == 0xe1) == Zero);
        EXPECT_EQ(S.EIP, Take ? 0x1022u : 0x1002u);
        EXPECT_EQ(S.Core.GPR[1], Count - 1);
        EXPECT_EQ(S.EFlags, 0xe97u | (Zero ? 64 : 0));
      }
}

TEST_F(X86LegacyExecutionTest, CounterZeroJumpDoesNotDecrement) {
  for (bool Address32 : {false, true}) {
    S.EIP = 0x1000;
    S.EFlags = 0x10ed7;
    S.Core.GPR[1] = 0xabcd0000;
    std::vector<uint8_t> Bytes;
    if (!Address32)
      Bytes.push_back(0x67);
    Bytes.insert(Bytes.end(), {0xe3, 0x20});
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(S.EIP, Address32 ? 0x1002u : 0x1023u);
    EXPECT_EQ(S.Core.GPR[1], 0xabcd0000u);
    EXPECT_EQ(S.EFlags, 0xed7u);
  }
}

TEST_F(X86LegacyExecutionTest, LoopTargetFaultRestartsTheOriginalCounter) {
  S.Core.Segments[1].Limit = 0x100f;
  S.Core.Segments[1].Attributes &= ~0x8000u;
  S.Core.GPR[1] = 2;
  ASSERT_EQ(run({0xe2, 0x20}).DeliveredVector, 13);
  EXPECT_EQ(S.Core.GPR[1], 2u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
}

TEST_F(X86LegacyExecutionTest, ConditionalScalarsAuthenticateBeforeEffects) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xf0, 0x0f, 0x44, 0xc0},
                                         {0xf3, 0x0f, 0x94, 0xc0},
                                         {0xf0, 0xe2, 0},
                                         {0x0f, 0x94},
                                         {0x0f, 0x44},
                                         {0xe3},
                                         {0xe1, 0, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
  }
  O.Scalars.reset();
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x0f, 0x44, 0xc1}, {0x0f, 0x94, 0xc0}, {0xe2, 0}, {0xe3, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}
