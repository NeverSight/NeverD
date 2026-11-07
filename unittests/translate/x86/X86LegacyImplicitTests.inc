//===- X86LegacyImplicitTests.inc - Implicit scalar operands --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact BSWAP, XLAT and LEAVE widths, registers and restart ownership.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, ByteSwapTouchesOnlyItsDwordRegister) {
  for (bool Code32 : {false, true})
    for (unsigned Reg = 0; Reg != 8; ++Reg) {
      S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
      S.Core.GPR.fill(0x12345678);
      S.EFlags = 0x10ed7;
      std::vector<uint8_t> Bytes{0x67, 0x64};
      if (!Code32)
        Bytes.push_back(0x66);
      Bytes.insert(Bytes.end(), {0x0f, uint8_t(0xc8 + Reg)});
      ASSERT_TRUE(run(Bytes).InstructionCompleted);
      for (unsigned I = 0; I != 8; ++I)
        EXPECT_EQ(S.Core.GPR[I], I == Reg ? 0x78563412u : 0x12345678u);
      EXPECT_EQ(S.EFlags, 0xed7u);
    }
}

TEST_F(X86LegacyExecutionTest, ByteSwapWordFormIsExplicitlyUnsupported) {
  for (bool Code32 : {false, true})
    for (unsigned Reg = 0; Reg != 8; ++Reg) {
      S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
      std::vector<uint8_t> Bytes;
      if (Code32)
        Bytes.push_back(0x66);
      Bytes.insert(Bytes.end(), {0x0f, uint8_t(0xc8 + Reg)});
      auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(S.EIP, 0x1000u);
      EXPECT_EQ(get(0x11004) & 0x20, 0u);
    }
}

TEST_F(X86LegacyExecutionTest, TableLookupUsesUnsignedALAndTruncatedSum) {
  for (unsigned I = 0; I != 256; ++I)
    put(uint16_t(0xff80 + I), I ^ 0xa5, GuestScalarWidth::I8);
  for (unsigned I = 0; I != 256; ++I) {
    S.Core.GPR[0] = 0x12345600 | I;
    S.Core.GPR[3] = 0xa5a5ff80;
    S.EFlags = 0x10ed7;
    ASSERT_TRUE(run({0x66, 0x67, 0xd7}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0x12345600u | (I ^ 0xa5));
    EXPECT_EQ(S.Core.GPR[3], 0xa5a5ff80u);
    EXPECT_EQ(S.EFlags, 0xed7u);
  }
}

TEST_F(X86LegacyExecutionTest, TableLookupOverridesTheDefaultDataSegment) {
  S.Core.Segments[3].Base = 0x10000;
  S.Core.Segments[4].Base = 0x8000;
  S.Core.GPR[3] = 0x9000;
  put(0x190ff, 0x81, GuestScalarWidth::I8);
  put(0x110ff, 0x7e, GuestScalarWidth::I8);
  S.Core.GPR[0] = 0xabcd00ff;
  ASSERT_TRUE(run({0xd7}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd0081u);
  S.Core.GPR[0] = 0xabcd00ff;
  ASSERT_TRUE(run({0x64, 0xd7}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0xabcd007eu);
}

TEST_F(X86LegacyExecutionTest, TableLookupReadsExactlyOneByteAndNeverWrites) {
  S.Core.GPR[3] = 0xcfff;
  S.Core.GPR[0] = 0x12345600;
  put(0xcfff, 0xa7, GuestScalarWidth::I8);
  put(0x11030, 0xc005, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  ASSERT_TRUE(run({0xd7}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[0], 0x123456a7u);
  EXPECT_EQ(get(0x11030) & 0x60, 0x20u);
  EXPECT_EQ(get(0x11034) & 0x60, 0u);
}

TEST_F(X86LegacyExecutionTest, TableLookupFaultRetainsALAndOriginalBase) {
  put(0x5804, 0xf000, GuestScalarWidth::I32);
  S.Core.GPR[3] = 0xcfff;
  S.Core.GPR[0] = 0x12345601;
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  const auto R = run({0xd7});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[0], 0x12345601u);
  EXPECT_EQ(S.Core.GPR[3], 0xcfffu);
  EXPECT_EQ(S.CR2, 0xd000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
}

TEST_F(X86LegacyExecutionTest, LeaveSeparatesStackFrameAndOperandWidths) {
  put(0xc100, 0x88776655, GuestScalarWidth::I32);
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u}) {
        S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
        S.Core.Segments[2].Attributes = Stack32 ? 0xc0f3 : 0x80f3;
        const uint32_t Frame = Stack32 ? 0xc100 : 0xabcdc100;
        S.Core.GPR[4] = 0x1234d100;
        S.Core.GPR[5] = Frame;
        S.EFlags = 0x10ed7;
        std::vector<uint8_t> Bytes{0x67, 0x64};
        if ((Width == 4) != Code32)
          Bytes.push_back(0x66);
        Bytes.push_back(0xc9);
        ASSERT_TRUE(run(Bytes).InstructionCompleted);
        EXPECT_EQ(S.Core.GPR[4], (Stack32 ? 0xc100u : 0x1234c100u) + Width);
        EXPECT_EQ(S.Core.GPR[5],
                  Width == 4 ? 0x88776655u : (Frame & 0xffff0000u) | 0x6655u);
        EXPECT_EQ(S.EFlags, 0xed7u);
      }
}

TEST_F(X86LegacyExecutionTest, LeaveFaultRestartsWithOriginalStackAndFrame) {
  put(0x5804, 0xf000, GuestScalarWidth::I32);
  S.Core.GPR[4] = 0xd100;
  S.Core.GPR[5] = 0xcfff;
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  const auto R = run({0xc9});
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[5], 0xcfffu);
  EXPECT_EQ(S.CR2, 0xd000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1000u);
  EXPECT_EQ(get(S.Core.GPR[4] + 16), 0xd100u);
}

TEST_F(X86LegacyExecutionTest, ImplicitOperandInstructionsRequireBinding) {
  O.Scalars.reset();
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x0f, 0xc8}, {0xd7}, {0xc9}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, ImplicitOperandGrammarRejectsMalformedInput) {
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x0f},
                                           {0x0f, 0xc8, 0},
                                           {0xf0, 0x0f, 0xc8},
                                           {0xf0, 0xd7},
                                           {0xf3, 0xc9},
                                           {0xd7, 0},
                                           {0xc9, 0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}
