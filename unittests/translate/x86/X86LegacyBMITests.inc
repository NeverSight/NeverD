//===- X86LegacyBMITests.inc - Protected BMI state contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent values, explicit features/undefined bits and fault ordering.
//===----------------------------------------------------------------------===//

class X86LegacyBMITest : public X86LegacyExecutionTest {
protected:
  void SetUp() override {
    X86LegacyExecutionTest::SetUp();
    O.IntegerExtensions = {true, true, true, true, true};
    O.BMIUndefinedFlags = 0;
    S.EFlags = 0xed7;
  }
};

TEST_F(X86LegacyBMITest, ValuesCodeWidthsAndEveryArithmeticFlagImage) {
  for (bool Code32 : {false, true})
    for (unsigned Form = 0; Form != test::LegacyBMIForms.size(); ++Form)
      for (unsigned Image = 0; Image != 64; ++Image)
        for (bool Choice : {false, true}) {
          const auto &F = test::LegacyBMIForms[Form];
          SCOPED_TRACE(F.Name);
          S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
          S.Core.GPR[0] = 0x87654321;
          S.Core.GPR[1] = F.Control;
          S.Core.GPR[2] = 0xffffffff;
          S.Core.GPR[6] = F.Source;
          S.EIP = 0x1000;
          S.EFlags = 0x602;
          constexpr unsigned Flags[]{0, 2, 4, 6, 7, 11};
          for (unsigned I = 0; I != 6; ++I)
            S.EFlags |= ((Image >> I) & 1) << Flags[I];
          const uint32_t Before = S.EFlags;
          O.BMIUndefinedFlags = Choice ? F.UndefinedMask : 0;
          ASSERT_TRUE(
              run(test::legacyBMIBytes(Form, 0, 1, 6)).InstructionCompleted);
          EXPECT_EQ(S.Core.GPR[0], F.Expected);
          EXPECT_EQ(S.Core.GPR[1], Form == 6 ? 1 : F.Control);
          EXPECT_EQ(S.EFlags, F.UndefinedMask ? 0x602 | F.DefinedFlags |
                                                    *O.BMIUndefinedFlags
                                              : Before);
        }
}

TEST_F(X86LegacyBMITest, ExtractAndIndexBoundariesUseLowControlBytes) {
  for (unsigned Form : {1u, 5u})
    for (unsigned Start : {0u, 1u, 31u, 32u, 33u, 255u})
      for (unsigned Length : {0u, 1u, 31u, 32u, 33u, 255u}) {
        S.EIP = 0x1000;
        S.Core.GPR[1] = 0xa5a50000 | (Length << 8) | Start;
        S.Core.GPR[6] = 0x80000003;
        uint32_t Expected = 0;
        // Select input bits directly, independently of the executor's shifts.
        for (unsigned Bit = 0; Bit != 32; ++Bit)
          if ((S.Core.GPR[6] >> Bit) & 1) {
            if (Form == 1 && Bit >= Start && Bit - Start < Length)
              Expected |= 1u << (Bit - Start);
            if (Form == 5 && Bit < Start)
              Expected |= 1u << Bit;
          }
        ASSERT_TRUE(
            run(test::legacyBMIBytes(Form, 0, 1, 6)).InstructionCompleted);
        EXPECT_EQ(S.Core.GPR[0], Expected);
        EXPECT_EQ(S.EFlags & 1, Form == 5 && Start >= 32);
      }
}

TEST_F(X86LegacyBMITest, MultiplyCapturesEDXAndBothAliasesBeforeWriting) {
  for (unsigned High : {0u, 1u, 2u, 6u})
    for (unsigned Low : {0u, 1u, 2u, 6u})
      for (unsigned Source : {0u, 1u, 2u, 6u}) {
        S.EIP = 0x1000;
        S.Core.GPR = {0xffffffff, 0x80000001, 0xfedcba98, 0, 0xd000, 0, 3, 0};
        auto Expected = S.Core.GPR;
        const uint64_t Product = uint64_t(Expected[2]) * Expected[Source];
        Expected[Low] = uint32_t(Product);
        Expected[High] = uint32_t(Product >> 32);
        ASSERT_TRUE(run(test::legacyBMIBytes(6, High, Low, Source))
                        .InstructionCompleted);
        EXPECT_EQ(S.Core.GPR, Expected);
        EXPECT_EQ(S.EFlags, 0xed7u);
      }
}

TEST_F(X86LegacyBMITest, FeaturesAreIndependentAndUDFollowsFetchBeforeData) {
  for (unsigned Form = 0; Form != test::LegacyBMIForms.size(); ++Form)
    for (bool FetchFault : {false, true}) {
      SetUp();
      O.IntegerExtensions->BMI1 = Form >= 5;
      O.IntegerExtensions->BMI2 = Form < 5;
      O.BMIUndefinedFlags.reset(); // Disabled operations never consume it.
      S.Core.GPR[3] = 0x9000;
      S.Core.GPR[0] = 0x12345678;
      S.Core.GPR[1] = 0x87654321;
      S.Core.Segments[3].Unusable = true;
      if (FetchFault)
        put(0x11004, 0, GuestScalarWidth::I32);
      const auto R = run(test::legacyBMIBytes(Form, 0, 1, -1));
      EXPECT_EQ(R.DeliveredVector, FetchFault ? 14 : 6);
      EXPECT_FALSE(R.InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
      EXPECT_EQ(S.Core.GPR[1], 0x87654321u);
      EXPECT_EQ(S.CR2, FetchFault ? 0x1000u : 0x12345678u);
      EXPECT_EQ(get(0x11024), 0x9007u);
    }
}

TEST_F(X86LegacyBMITest, MissingOrDefinedFlagChoicesRefuseBeforeFetch) {
  for (unsigned Form = 0; Form != 6; ++Form)
    for (unsigned Case = 0; Case != 4; ++Case) {
      SetUp();
      if (!Case)
        O.IntegerExtensions.reset();
      else if (Case == 1)
        O.BMIUndefinedFlags.reset();
      else
        O.BMIUndefinedFlags = Case == 2 ? 1 : 0x800;
      auto R = executeX86LegacyInstruction(
          S, *M, test::legacyBMIBytes(Form, 0, 1, 6), O);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(S.EIP, 0x1000u);
      EXPECT_EQ(S.EFlags, 0xed7u);
      EXPECT_EQ(get(0x11004), 0x1007u);
    }
  SetUp();
  O.BMIUndefinedFlags.reset();
  ASSERT_TRUE(run(test::legacyBMIBytes(6, 0, 1, 6)).InstructionCompleted);
}

TEST_F(X86LegacyBMITest, StrictVEXGrammarDoesNotAdmitOtherInstructions) {
  for (unsigned Form = 0; Form != test::LegacyBMIForms.size(); ++Form)
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      // This six-byte RORX mutation is an exact legacy LES with disp32.
      // The shared dispatcher must retain that independently valid grammar.
      if (Form == 9 && Mutation == 1)
        continue;
      auto Bytes = test::legacyBMIBytes(Form, 0, 1, 6);
      switch (Mutation) {
      case 0:
        Bytes[1] &= ~0x80;
        break; // LES, not VEX.
      case 1:
        Bytes[1] &= ~0x40;
        break;
      case 2:
        Bytes[2] |= 4;
        break; // Reserved L.
      case 3:
        Bytes[1] = 0xe1;
        break; // Wrong map.
      case 4:
        Bytes.insert(Bytes.begin(), 0x66);
        break;
      case 5:
        Bytes.insert(Bytes.begin(), 0xf2);
        break;
      case 6:
        Bytes.insert(Bytes.begin(), 0xf3);
        break;
      case 7:
        Bytes.insert(Bytes.begin(), 0xf0);
        break;
      case 8:
        Bytes.pop_back();
        break;
      case 9:
        Bytes.push_back(0);
        break;
      }
      auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
      ASSERT_FALSE(bool(R)) << testing::PrintToString(Bytes);
      llvm::consumeError(R.takeError());
      EXPECT_EQ(get(0x11004), 0x1007u);
    }
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xc4, 0xe2, 0x78, 0xf3, 0xc6}, // Wrong group.
        {0xc4, 0xe2, 0x79, 0xf5, 0xc6},                     // Reserved PP.
        {0xc4, 0xe3, 0x73, 0xf0, 0xc6, 1}, // RORX reserved vvvv.
        {0x67, 0x67, 0xc4, 0xe2, 0x78, 0xf7, 0xc6}}) {
    auto E = validateX86LegacyInstructionEncoding(Bytes, true);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}

TEST_F(X86LegacyBMITest, C4LegacyLESIsIndependentOfBMIFeatures) {
  O.IntegerExtensions.reset();
  O.BMIUndefinedFlags.reset();
  S.Core.GPR[3] = 0x9000u - 0x01c6f07bu;
  put(0x9000, 0x11223344, GuestScalarWidth::I32);
  put(0x9004, 35, GuestScalarWidth::I16);
  ASSERT_TRUE(run({0xc4, 0xa3, 0x7b, 0xf0, 0xc6, 1}).InstructionCompleted);
  EXPECT_EQ(S.Core.GPR[4], 0x11223344u);
  EXPECT_EQ(S.Core.Segments[0].Selector, 35);
}

TEST_F(X86LegacyBMITest, BackingStopPreservesBothDestinationsAndFlags) {
  for (unsigned Form = 0; Form != test::LegacyBMIForms.size(); ++Form) {
    SetUp();
    S.Core.GPR[0] = 0x12345678;
    S.Core.GPR[1] = 0x87654321;
    S.Core.GPR[3] = 0x9000;
    put(0x9000, 0x8101, GuestScalarWidth::I32);
    put(0x11024, 0x40007, GuestScalarWidth::I32);
    const auto Bytes = test::legacyBMIBytes(Form, 0, 1, -1);
    ASSERT_EQ(run(Bytes).Status, X86LegacyExecutionStatus::BackingFault);
    EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
    EXPECT_EQ(S.Core.GPR[1], 0x87654321u);
    EXPECT_EQ(S.EFlags, 0xed7u);
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_FALSE(S.PendingEvent);
    put(0x11024, 0x9007, GuestScalarWidth::I32);
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
  }
}

TEST_F(X86LegacyBMITest, ReadWatchpointsFollowCommitAndNeverSurviveFaults) {
  for (unsigned Form = 0; Form != test::LegacyBMIForms.size(); ++Form)
    for (bool Fault : {false, true}) {
      SetUp();
      O.Debug = X86LegacyDebugProfile::Watchpoints;
      S.Debug.emplace();
      S.Debug->DR7 = 0x30401;
      S.Debug->DR[0] = 0x9000;
      S.Debug->DR6 = 0xffff0ff0;
      S.Core.GPR[3] = 0x9000;
      if (Fault)
        put(0x11024, 0, GuestScalarWidth::I32);
      const auto Bytes = test::legacyBMIBytes(Form, 0, 1, -1);
      const auto R = run(Bytes);
      EXPECT_EQ(R.DeliveredVector, Fault ? 14 : 1);
      EXPECT_EQ(R.InstructionCompleted, !Fault);
      EXPECT_EQ(S.Debug->DR6, Fault ? 0xffff0ff0u : 0xffff0ff1u);
      EXPECT_EQ(get(S.Core.GPR[4] + (Fault ? 4 : 0)),
                Fault ? 0x1000u : 0x1000u + Bytes.size());
    }
}
