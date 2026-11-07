//===- X86LegacyDoubleShiftTests.inc - Protected SHLD/SHRD contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bit transfers, undefined slices and restartable memory faults.
//===----------------------------------------------------------------------===//

TEST_F(X86LegacyExecutionTest, DoubleShiftEveryByteCountUsesDefinedBits) {
  for (unsigned Width : {2u, 4u})
    for (bool Right : {false, true})
      for (bool Immediate : {false, true})
        for (unsigned Raw = 0; Raw != 256; ++Raw)
          for (uint32_t Image : {0u, 0x8d5u}) {
            const unsigned Bits = Width * 8, Count = Raw & 31;
            const uint32_t Mask = UINT32_MAX >> (32 - Bits);
            const uint32_t Sign = (Mask >> 1) + 1;
            uint32_t Value = 0x81a5c37f & Mask, Source = 0x12345678 & Mask;
            uint32_t Flags = 0xed7;
            bool Carry = true;
            // Independent single-bit transfer instead of the executor's
            // concatenated-pair calculation. Out-of-range word results use
            // an arbitrary image, never this recurrence.
            if (Count > Bits) {
              Value = Image ? 0xa55a : 0x5aa5;
              Flags = (Flags & ~0x8d5u) | Image;
            } else if (Count) {
              for (unsigned I = 0; I != Count; ++I) {
                Carry = Value & (Right ? 1 : Sign);
                Value = Right ? (Value >> 1) | ((Source & 1) ? Sign : 0)
                              : ((Value << 1) | bool(Source & Sign)) & Mask;
                Source = Right ? Source >> 1 : (Source << 1) & Mask;
              }
              unsigned Ones = 0;
              for (unsigned Bit = 0; Bit != 8; ++Bit)
                Ones += (Value >> Bit) & 1;
              Flags = (Flags & ~0x8d5u) | unsigned(Carry) |
                      (Ones % 2 == 0 ? 4 : 0) | (Image & 16) |
                      (!Value ? 64 : 0) | (Value & Sign ? 128 : 0) |
                      (Count == 1 ? ((0x81a5c37f ^ Value) & Sign ? 0x800 : 0)
                                  : Image & 0x800);
            }
            S.EIP = 0x1000;
            S.EFlags = 0x10ed7;
            S.Core.GPR[0] = 0x81a5c37f;
            S.Core.GPR[1] = Raw;
            S.Core.GPR[2] = 0x12345678;
            O.DoubleShiftUndefined = {uint16_t(Image ? 0xa55a : 0x5aa5), Image};
            std::vector<uint8_t> Bytes;
            if (Width == 2)
              Bytes.push_back(0x66);
            Bytes.insert(
                Bytes.end(),
                {0x0f, uint8_t((Right ? 0xac : 0xa4) + !Immediate), 0xd0});
            if (Immediate)
              Bytes.push_back(Raw);
            ASSERT_TRUE(run(Bytes).InstructionCompleted);
            EXPECT_EQ(S.Core.GPR[0], (0x81a5c37f & ~Mask) | Value);
            EXPECT_EQ(S.Core.GPR[2], 0x12345678u);
            EXPECT_EQ(S.EFlags, Flags);
          }
}

TEST_F(X86LegacyExecutionTest, DoubleShiftCountAtWidthIsStillDefined) {
  for (bool Right : {false, true}) {
    S.Core.GPR[0] = 0xabcd8001;
    S.Core.GPR[2] = 0x24680002;
    S.EFlags = 0x202;
    O.DoubleShiftUndefined = {0xffff, 0x8d5};
    ASSERT_TRUE(run({0x66, 0x0f, uint8_t(Right ? 0xac : 0xa4), 0xd0, 16})
                    .InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[0], 0xabcd0002u);
    EXPECT_EQ(S.EFlags, 0xa13u);
  }
}

TEST_F(X86LegacyExecutionTest, DoubleShiftCapturesAliasedSourceAndCL) {
  O.DoubleShiftUndefined = {0xffff, 0};
  for (bool Right : {false, true}) {
    S.Core.GPR[1] = 0x80000001;
    S.EFlags = 0x202;
    ASSERT_TRUE(
        run({0x0f, uint8_t(Right ? 0xad : 0xa5), 0xc9}).InstructionCompleted);
    EXPECT_EQ(S.Core.GPR[1], Right ? 0xc0000000u : 3u);
    EXPECT_EQ(S.EFlags, Right ? 0x287u : 0xa07u);
  }
}

TEST_F(X86LegacyExecutionTest, DoubleShiftCodeOperandAndAddressWidths) {
  O.DoubleShiftUndefined = {0xffff, 0};
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u}) {
      S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
      S.Core.GPR[0] = 0x80008001;
      S.Core.GPR[2] = 0x80008000;
      S.EFlags = 0x202;
      std::vector<uint8_t> Bytes{0x67, 0x64};
      if ((Width == 4) != Code32)
        Bytes.push_back(0x66);
      Bytes.insert(Bytes.end(), {0x0f, 0xa4, 0xd0, 1});
      ASSERT_TRUE(run(Bytes).InstructionCompleted);
      EXPECT_EQ(S.Core.GPR[0], Width == 2 ? 0x80000003u : 0x00010003u);
      EXPECT_EQ(S.EFlags, 0xa07u);
    }
}

TEST_F(X86LegacyExecutionTest, DoubleShiftZeroAndUndefinedCountsStillFault) {
  O.DoubleShiftUndefined = {0xffff, 0x8d5};
  for (uint8_t Count : {0, 17, 32}) {
    SetUp();
    S.Core.GPR[3] = 0xc100;
    S.EFlags = 0xed7;
    put(0xc100, 0x12345678);
    put(0x11030, 0xc005, GuestScalarWidth::I32);
    const auto R = run({0x66, 0x0f, 0xa4, 0x13, Count});
    EXPECT_FALSE(R.InstructionCompleted);
    EXPECT_EQ(R.DeliveredVector, 14);
    EXPECT_EQ(S.CR2, 0xc100u);
    EXPECT_EQ(get(S.Core.GPR[4]), 7u);
    EXPECT_EQ(get(S.Core.GPR[4] + 12) & 0x8d5, 0x8d5u);
    EXPECT_EQ(get(0xc100), 0x12345678u);
  }
}

TEST_F(X86LegacyExecutionTest, DoubleShiftCrossPageFaultKeepsOriginalState) {
  O.DoubleShiftUndefined = {0xffff, 0};
  put(0x5804, 0xf000, GuestScalarWidth::I32);
  put(0xcfff, 0x87654321, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  S.Core.GPR[3] = 0xcfff;
  S.Core.GPR[2] = 0x12345678;
  S.EFlags = 0xed7;
  const auto R = run({0x66, 0x0f, 0xac, 0x13, 17});
  EXPECT_FALSE(R.InstructionCompleted);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0xd000u);
  EXPECT_EQ(get(0xcfff), 0x87654321u);
  EXPECT_EQ(S.Core.GPR[2], 0x12345678u);
  EXPECT_EQ(get(S.Core.GPR[4] + 12) & 0x8d5, 0x8d5u);
  EXPECT_EQ(get(S.Core.GPR[4]), 6u);
}

TEST_F(X86LegacyExecutionTest, DoubleShiftBindingRefusesBeforeFetch) {
  for (std::optional<X86LegacyDoubleShiftUndefinedState> Image :
       {std::optional<X86LegacyDoubleShiftUndefinedState>{},
        std::optional<X86LegacyDoubleShiftUndefinedState>{{0, 0x10000}}}) {
    O.DoubleShiftUndefined = Image;
    auto R = executeX86LegacyInstruction(S, *M, {0x0f, 0xa5, 0xd0}, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}

TEST_F(X86LegacyExecutionTest, DoubleShiftMalformedFormsRefuseBeforeFetch) {
  O.DoubleShiftUndefined = {0, 0};
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x0f, 0xa4},
                                           {0x0f, 0xa4, 0xd0},
                                           {0x0f, 0xad, 0xd0, 0},
                                           {0xf0, 0x0f, 0xa5, 0xd0},
                                           {0xf3, 0x0f, 0xad, 0xd0},
                                           {0x66, 0x66, 0x0f, 0xa5, 0xd0}}) {
    auto R = executeX86LegacyInstruction(S, *M, Bytes, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(get(0x11004) & 0x20, 0u);
  }
}
