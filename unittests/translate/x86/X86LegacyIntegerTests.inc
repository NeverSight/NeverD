//===- X86LegacyIntegerTests.inc - Protected counts and carry chains -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Target feature selection, independent values/flags and fault publication.
//===----------------------------------------------------------------------===//

class X86LegacyIntegerTest : public X86LegacyExecutionTest {
protected:
  void SetUp() override {
    X86LegacyExecutionTest::SetUp();
    O.IntegerExtensions = {true, true, true, true};
    O.BitCountUndefinedFlags = 0;
    S.EFlags = 0xed7;
  }
};

TEST_F(X86LegacyIntegerTest, CountWidthsResultsAndIndependentFlagChoices) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (uint8_t Opcode : {0xb8, 0xbc, 0xbd})
        for (uint32_t Value : {0u, 1u, 2u, 0x8000u, 0x10000u, 0x80000000u,
                               0xffffffffu, 0x12345678u})
          for (uint32_t Choice : {0u, kX86LegacyBitCountUndefinedFlagsMask}) {
            S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
            S.Core.GPR[0] = 0xaabbccdd;
            S.Core.GPR[1] = Value;
            S.EIP = 0x1000;
            S.EFlags = 0xed7;
            O.BitCountUndefinedFlags = Choice;
            std::vector<uint8_t> Bytes{0xf3, 0x0f, Opcode, 0xc1};
            if ((Width == 4) != Code32)
              Bytes.insert(Bytes.begin(), 0x66);
            unsigned Count = 0;
            const uint32_t Source = Width == 2 ? uint16_t(Value) : Value;
            for (unsigned I = 0; I != Width * 8; ++I) {
              const unsigned Bit = Opcode == 0xbd ? Width * 8 - I - 1 : I;
              const bool Set = (Source >> Bit) & 1;
              if (Opcode == 0xb8)
                Count += Set;
              else if (!Set)
                ++Count;
              else
                break;
            }
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.Core.GPR[0], (Width == 2 ? 0xaabb0000 : 0) | Count);
            EXPECT_EQ(S.Core.GPR[1], Value);
            EXPECT_EQ(S.EFlags, (0xed7 & ~kX86LegacyArithmeticFlagsMask) |
                                    (Opcode == 0xb8 ? 0 : Choice) |
                                    (!Count ? 64 : 0) |
                                    (Opcode != 0xb8 && !Source ? 1 : 0));
          }
}

TEST_F(X86LegacyIntegerTest, ADXAlwaysUsesDwordsAndOneUnsignedCarryChain) {
  constexpr unsigned FlagBits[]{0, 2, 4, 6, 7, 11};
  for (bool Code32 : {false, true})
    for (bool Overflow : {false, true})
      for (unsigned Image = 0; Image != 64; ++Image)
        for (uint32_t Left : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
          for (uint32_t Right :
               {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
            S.Core.GPR[0] = Left;
            S.Core.GPR[1] = Right;
            S.EIP = 0x1000;
            uint32_t Flags = 0x602;
            for (unsigned I = 0; I != 6; ++I)
              Flags |= ((Image >> I) & 1) << FlagBits[I];
            S.EFlags = Flags;
            const uint32_t CarryBit = Overflow ? 0x800 : 1;
            const uint64_t Sum =
                uint64_t(Left) + Right + bool(Flags & CarryBit);
            std::vector<uint8_t> Bytes{uint8_t(Overflow ? 0xf3 : 0x66), 0x0f,
                                       0x38, 0xf6, 0xc1};
            // ADOX ignores 66 in either order, even in 16-bit code.
            if (Overflow)
              Bytes.insert(Bytes.begin() + (Image & 1), 0x66);
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.Core.GPR[0], uint32_t(Sum));
            EXPECT_EQ(S.EFlags,
                      (Flags & ~CarryBit) | (Sum > UINT32_MAX ? CarryBit : 0));
          }
}

TEST_F(X86LegacyIntegerTest, DisabledZeroCountFeaturesSelectOriginalScans) {
  O.IntegerExtensions = {true, false, false, true};
  O.BitCountUndefinedFlags.reset();
  for (uint8_t Opcode : {0xbc, 0xbd})
    for (uint32_t Source : {0u, 1u, 0x8100u, 0x80000000u}) {
      S.EIP = 0x1000;
      S.EFlags = 0x602;
      S.Core.GPR[0] = Source; // Destructive alias must capture the source.
      O.BitScanChoices = {0x12345678, 0x815};
      ASSERT_TRUE(run({0xf3, 0x0f, Opcode, 0xc0}).InstructionCompleted);
      unsigned Index = 0;
      if (Source) {
        for (unsigned I = 0; I != 32; ++I)
          if ((Source >> I) & 1) {
            Index = I;
            if (Opcode == 0xbc)
              break;
          }
      }
      EXPECT_EQ(S.Core.GPR[0], Source ? Index : 0x12345678u);
      EXPECT_EQ(S.EFlags, 0x602u | 0x815 | (!Source ? 64 : 0));
    }
}

TEST_F(X86LegacyIntegerTest, UnavailableInstructionsFaultAfterFetchBeforeData) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xf3, 0x0f, 0xb8, 0x07},
        {0x66, 0x0f, 0x38, 0xf6, 0x07},
        {0xf3, 0x0f, 0x38, 0xf6, 0x07}})
    for (bool FetchFault : {false, true}) {
      SetUp();
      O.IntegerExtensions.emplace();
      S.Core.GPR[0] = 0x12345678;
      S.Core.GPR[7] = 0x9000;
      S.Core.Segments[3].Unusable = true;
      if (FetchFault)
        put(0x11004, 0, GuestScalarWidth::I32);
      const auto R = run(Bytes);
      EXPECT_EQ(R.DeliveredVector, FetchFault ? 14 : 6);
      EXPECT_FALSE(R.InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
      EXPECT_EQ(S.CR2, FetchFault ? 0x1000u : 0x12345678u);
      EXPECT_EQ(get(0x11024), 0x9007u); // No operand walk on #UD.
    }
}

TEST_F(X86LegacyIntegerTest,
       MissingFeatureAndUndefinedChoicesRefuseBeforeFetch) {
  for (unsigned Case = 0; Case != 5; ++Case) {
    SetUp();
    if (!Case)
      O.IntegerExtensions.reset();
    else if (Case == 1)
      O.BitCountUndefinedFlags.reset();
    else if (Case == 2)
      O.BitCountUndefinedFlags = 1; // Defined CF cannot be supplied.
    else {
      O.IntegerExtensions->BMI1 = false;
      O.BitScanChoices.reset();
      if (Case == 4)
        O.BitScanChoices = {0, 64}; // Defined ZF cannot be supplied.
    }
    auto R = executeX86LegacyInstruction(S, *M, {0xf3, 0x0f, 0xbc, 0xc1}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(S.EFlags, 0xed7u);
    EXPECT_EQ(get(0x11004), 0x1007u);
  }
}

TEST_F(X86LegacyIntegerTest, ExactGrammarRejectsWrongSelectorsAndExtents) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x0f, 0xb8, 0xc1},
        {0x0f, 0x38, 0xf6, 0xc1},
        {0xf2, 0x0f, 0xb8, 0xc1},
        {0xf3, 0xf3, 0x0f, 0xbc, 0xc1},
        {0x66, 0x66, 0x0f, 0x38, 0xf6, 0xc1},
        {0xf0, 0x66, 0x0f, 0x38, 0xf6, 0x01},
        {0xf3, 0x0f, 0xbd},
        {0xf3, 0x0f, 0xb8, 0xc1, 0},
        {0xf3, 0x01, 0xc1}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R)) << testing::PrintToString(Bytes);
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004), 0x1007u);
  }
}

TEST_F(X86LegacyIntegerTest,
       OperandBackingStopDoesNotPublishDestinationOrFlags) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xf3, 0x0f, 0xb8, 0x07},
        {0xf3, 0x0f, 0xbc, 0x07},
        {0xf3, 0x0f, 0xbd, 0x07},
        {0x66, 0x0f, 0x38, 0xf6, 0x07},
        {0xf3, 0x0f, 0x38, 0xf6, 0x07}}) {
    SetUp();
    S.Core.GPR[0] = 0xffffffff;
    S.Core.GPR[7] = 0x9000;
    put(0x9000, 1, GuestScalarWidth::I32);
    put(0x11024, 0x40007, GuestScalarWidth::I32);
    const auto R = run(Bytes);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_EQ(S.Core.GPR[0], 0xffffffffu);
    EXPECT_EQ(S.EFlags, 0xed7u);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_FALSE(S.PendingEvent);
    put(0x11024, 0x9007, GuestScalarWidth::I32);
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_NE(S.Core.GPR[0], 0xffffffffu);
  }
}

TEST_F(X86LegacyIntegerTest, ReadWatchpointPublishesAfterCountButNotFault) {
  for (bool Fault : {false, true}) {
    SetUp();
    O.Debug = X86LegacyDebugProfile::Watchpoints;
    S.Debug.emplace();
    S.Debug->DR7 = 0x30401;
    S.Debug->DR[0] = 0x9000;
    S.Debug->DR6 = 0xffff0ff0;
    S.Core.GPR[0] = 0x12345678;
    S.Core.GPR[7] = 0x9000;
    put(0x9000, 0x8101, GuestScalarWidth::I32);
    if (Fault)
      put(0x11024, 0, GuestScalarWidth::I32);
    const auto R = run({0xf3, 0x0f, 0xb8, 0x07});
    EXPECT_EQ(R.DeliveredVector, Fault ? 14 : 1);
    EXPECT_EQ(R.InstructionCompleted, !Fault);
    EXPECT_EQ(S.Core.GPR[0], Fault ? 0x12345678u : 3u);
    EXPECT_EQ(S.Debug->DR6, Fault ? 0xffff0ff0u : 0xffff0ff1u);
    EXPECT_EQ(get(S.Core.GPR[4] + (Fault ? 4 : 0)), Fault ? 0x1000u : 0x1004u);
  }
}
