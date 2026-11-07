//===- X86LegacyBitTests.inc - Protected bit operations ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bit strings, scan choices, complete operand bounds and fault rollback.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, BitTestEveryImmediateAndRegisterIndex) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (unsigned Operation = 0; Operation != 4; ++Operation)
        for (bool Immediate : {false, true})
          for (unsigned Index = 0; Index != 256; ++Index)
            for (uint32_t Image : {0u, 0x894u}) {
              const unsigned Bit = Index % (Width * 8);
              const uint32_t Original = 0x87654321, Mask = uint32_t(1) << Bit;
              uint32_t Value = Original;
              if (Operation == 1)
                Value |= Mask;
              if (Operation == 2)
                Value &= ~Mask;
              if (Operation == 3)
                Value ^= Mask;
              S.EIP = 0x1000;
              S.EFlags = 0x10ed7;
              S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
              S.Core.GPR[0] = Original;
              S.Core.GPR[1] = 0xabc00000 | Index;
              O.BitTestUndefinedFlags = Image;
              std::vector<uint8_t> Bytes{0x67, 0x64};
              if ((Width == 4) != Code32)
                Bytes.push_back(0x66);
              Bytes.insert(Bytes.end(),
                           {0x0f,
                            uint8_t(Immediate ? 0xba : 0xa3 + 8 * Operation),
                            uint8_t(Immediate ? 0xe0 + 8 * Operation : 0xc8)});
              if (Immediate)
                Bytes.push_back(Index);
              ASSERT_TRUE(run(Bytes).InstructionCompleted);
              EXPECT_EQ(S.Core.GPR[0], Value);
              EXPECT_EQ(S.Core.GPR[1], 0xabc00000u | Index);
              EXPECT_EQ(S.EFlags, (0xed7u & ~0x895u) | Image |
                                      unsigned(bool(Original & Mask)));
            }
}

TEST_F(X86LegacyExecutionTest, BitTestCapturesAliasedRegisterIndex) {
  O.BitTestUndefinedFlags = 0;
  for (unsigned Operation = 0; Operation != 4; ++Operation) {
    S.Core.GPR[1] = 0x8000001f;
    ASSERT_TRUE(
        run({0x0f, uint8_t(0xa3 + 8 * Operation), 0xc9}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[1], Operation < 2 ? 0x8000001fu : 31u);
    EXPECT_EQ(S.EFlags & 1, 1u);
  }
}

TEST_F(X86LegacyExecutionTest, BitStringSignedIndicesSelectWholeWords) {
  O.BitTestUndefinedFlags = 0;
  for (unsigned Width : {2u, 4u})
    for (int Index : {-65, -33, -32, -17, -16, -1, 0, 15, 16, 31, 32, 65}) {
      const int Bits = Width * 8;
      const int Word = (Index < 0 ? Index - Bits + 1 : Index) / Bits;
      const unsigned Bit = (Index % Bits + Bits) % Bits;
      const uint32_t Address = 0xc100 + Word * int(Width);
      for (unsigned Operation = 0; Operation != 4; ++Operation) {
        S.Core.GPR[3] = 0xc100;
        S.Core.GPR[1] =
            Width == 2 ? 0x12340000u | uint16_t(Index) : uint32_t(Index);
        put(0xc100, 0);
        put(Address, uint32_t(1) << Bit,
            Width == 2 ? GuestScalarWidth::I16 : GuestScalarWidth::I32);
        std::vector<uint8_t> Bytes;
        if (Width == 2)
          Bytes.push_back(0x66);
        Bytes.insert(Bytes.end(), {0x0f, uint8_t(0xa3 + 8 * Operation), 0x0b});
        ASSERT_TRUE(run(Bytes).InstructionCompleted);
        EXPECT_EQ(S.EFlags & 1, 1u);
        EXPECT_EQ(get(Address, Width == 2 ? GuestScalarWidth::I16
                                          : GuestScalarWidth::I32),
                  Operation < 2 ? uint32_t(1) << Bit : 0u);
        if (Address != 0xc100)
          EXPECT_EQ(get(0xc100, Width == 2 ? GuestScalarWidth::I16
                                           : GuestScalarWidth::I32),
                    0u);
      }
    }
}

TEST_F(X86LegacyExecutionTest, BitStringImmediateHighBitsDoNotExtendAddress) {
  O.BitTestUndefinedFlags = 0;
  S.Core.GPR[3] = 0xc100;
  put(0xc100, 0x80008000);
  for (unsigned Width : {2u, 4u}) {
    std::vector<uint8_t> Bytes;
    if (Width == 2)
      Bytes.push_back(0x66);
    Bytes.insert(Bytes.end(), {0x0f, 0xba, 0x23, 255});
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(S.EFlags & 1, 1u);
  }
}

TEST_F(X86LegacyExecutionTest, BitStringAddressWrapPrecedesSegmentBase) {
  O.BitTestUndefinedFlags = 0;
  for (unsigned Width : {2u, 4u}) {
    S.Core.GPR[3] = Width == 2 ? 0x12340000 : 0x1234fffc;
    S.Core.GPR[1] = Width == 2 ? 0xffffffff : 32;
    S.Core.Segments[4].Base = 0x8000;
    put(Width == 2 ? 0x17ffe : 0x8000, Width == 2 ? 0x8000 : 1,
        Width == 2 ? GuestScalarWidth::I16 : GuestScalarWidth::I32);
    std::vector<uint8_t> Bytes{0x67, 0x64};
    if (Width == 2)
      Bytes.push_back(0x66);
    Bytes.insert(Bytes.end(), {0x0f, 0xa3, 0x0f});
    ASSERT_TRUE(run(Bytes).InstructionCompleted);
    EXPECT_EQ(S.EFlags & 1, 1u);
  }
}

TEST_F(X86LegacyExecutionTest, BitTestReadsButUpdatesRequireWritePermission) {
  for (unsigned Operation = 0; Operation != 4; ++Operation) {
    SetUp();
    O.BitTestUndefinedFlags = 0;
    S.EFlags = 0xed7;
    S.Core.GPR[3] = 0xc100;
    S.Core.GPR[1] = 0;
    put(0xc100, 1);
    put(0x11030, 0xc005, GuestScalarWidth::I32);
    const auto R = run({0x0f, uint8_t(0xa3 + 8 * Operation), 0x0b});
    EXPECT_EQ(R.InstructionCompleted, Operation == 0);
    EXPECT_EQ(get(0xc100), 1u);
    if (Operation) {
      EXPECT_EQ(R.DeliveredVector, 14);
      EXPECT_EQ(S.CR2, 0xc100u);
      EXPECT_EQ(get(S.Core.GPR[4]), 7u);
      EXPECT_EQ(get(S.Core.GPR[4] + 12) & 0x8d5, 0x8d5u);
    }
  }
}

TEST_F(X86LegacyExecutionTest, BitStringFullWidthAccessCanCrossPage) {
  for (unsigned Operation = 0; Operation != 4; ++Operation) {
    SetUp();
    O.BitTestUndefinedFlags = 0x894;
    put(0x5804, 0xf000, GuestScalarWidth::I32);
    S.EFlags = 0xed7;
    S.Core.GPR[3] = 0xcfff;
    S.Core.GPR[1] = 0;
    put(0xcfff, 0x12345678);
    put(0x11034, 0xd006, GuestScalarWidth::I32);
    const auto R = run({0x66, 0x0f, uint8_t(0xa3 + 8 * Operation), 0x0b});
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, 0xd000u);
    EXPECT_EQ(get(0xcfff), 0x12345678u);
    EXPECT_EQ(get(S.Core.GPR[4]), Operation == 0 ? 4u : 6u);
    EXPECT_EQ(get(S.Core.GPR[4] + 12) & 0x8d5, 0x8d5u);
  }
}

TEST_F(X86LegacyExecutionTest, BitScanIndicesAndZFIgnoreCPUChoices) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (bool Reverse : {false, true})
        for (unsigned Bit = 0; Bit != Width * 8; ++Bit)
          for (uint32_t Flags : {0u, 0x895u}) {
            const uint32_t Low = uint32_t(1) << Bit;
            const uint32_t High = uint32_t(1) << (Width * 8 - 1);
            S.EIP = 0x1000;
            S.EFlags = 0x10ed7;
            S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
            S.Core.GPR[0] = 0xa5a55a5a;
            S.Core.GPR[1] = (Width == 2 ? 0x12340000 : 0) | Low | High;
            O.BitScanChoices = {0xffffffff, Flags};
            std::vector<uint8_t> Bytes{0x67, 0x65};
            if ((Width == 4) != Code32)
              Bytes.push_back(0x66);
            Bytes.insert(Bytes.end(),
                         {0x0f, uint8_t(Reverse ? 0xbd : 0xbc), 0xc1});
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.Core.GPR[0], (Width == 2 ? 0xa5a50000u : 0) |
                                         (Reverse ? Width * 8 - 1 : Bit));
            EXPECT_EQ(S.EFlags, 0x602u | Flags);
          }
}

TEST_F(X86LegacyExecutionTest, BitScanZeroResultAndAliasedSource) {
  for (unsigned Width : {2u, 4u})
    for (bool Reverse : {false, true})
      for (uint32_t Result : {0u, 0x98765432u}) {
        S.Core.GPR[1] = Width == 2 ? 0xabcd0000 : 0;
        O.BitScanChoices = {Result, 0};
        std::vector<uint8_t> Bytes;
        if (Width == 2)
          Bytes.push_back(0x66);
        Bytes.insert(Bytes.end(), {0x0f, uint8_t(Reverse ? 0xbd : 0xbc), 0xc9});
        ASSERT_TRUE(run(Bytes).InstructionCompleted);
        EXPECT_EQ(S.Core.GPR[1],
                  Width == 2 ? 0xabcd0000u | uint16_t(Result) : Result);
        EXPECT_EQ(S.EFlags & 0x8d5, 64u);
        S.Core.GPR[1] = 0x80008000;
        ASSERT_TRUE(run(Bytes).InstructionCompleted);
        EXPECT_EQ(S.Core.GPR[1], Width == 2 ? 0x8000000fu
                                 : Reverse  ? 31u
                                            : 15u);
        EXPECT_EQ(S.EFlags & 0x8d5, 0u);
      }
}

TEST_F(X86LegacyExecutionTest,
       BitScanFaultPreservesOriginalDestinationAndFlags) {
  O.BitScanChoices = {0x98765432, 0};
  put(0x5804, 0xf000, GuestScalarWidth::I32);
  S.Core.GPR[0] = 0x12345678;
  S.Core.GPR[3] = 0xcfff;
  S.EFlags = 0xed7;
  put(0xcfff, 0);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  const auto R = run({0x66, 0x0f, 0xbc, 0x03});
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xd000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 4u);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(get(S.Core.GPR[4] + 12) & 0x8d5, 0x8d5u);
}

TEST_F(X86LegacyExecutionTest, BitBindingsRefuseBeforeFetch) {
  for (bool Scan : {false, true})
    for (bool Invalid : {false, true}) {
      O.BitTestUndefinedFlags.reset();
      O.BitScanChoices.reset();
      if (Invalid) {
        O.BitTestUndefinedFlags = 0x40;
        O.BitScanChoices = {0, 0x40};
      }
      auto R = executeX86LegacyInstruction(
          S, *M, {0x0f, uint8_t(Scan ? 0xbc : 0xa3), 0xc1}, O);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(S.EIP, 0x1000u);
      EXPECT_EQ(get(0x11004) & 0x20, 0u);
    }
}

TEST_F(X86LegacyExecutionTest, BitMalformedAndUnboundAtomicFormsRefuse) {
  O.BitTestUndefinedFlags = 0;
  O.BitScanChoices = {0, 0};
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x0f, 0xa3},
                                           {0x0f, 0xba, 0xe0},
                                           {0x0f, 0xba, 0xd8, 1},
                                           {0x0f, 0xbc},
                                           {0x0f, 0xbd, 0xc0, 0},
                                           {0xf0, 0x0f, 0xab, 0x0b},
                                           {0xf0, 0x0f, 0xa3, 0x0b},
                                           {0xf3, 0x0f, 0xbc, 0xc0},
                                           {0xf3, 0x0f, 0xbd, 0xc0},
                                           {0x66, 0x66, 0x0f, 0xa3, 0xc0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}
