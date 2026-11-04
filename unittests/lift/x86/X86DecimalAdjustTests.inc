//===- X86DecimalAdjustTests.inc - Decimal values and undefined audit ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify original immediate ownership and exact arbitrary-output metadata.
//===----------------------------------------------------------------------===//

TEST(X86DecimalAdjust, EveryBaseAndDecoderRoutePreservesDefinedValues) {
  unsigned Cases = 0;
  for (uint8_t Opcode : {0xd4, 0xd5})
    for (unsigned Base = Opcode == 0xd4 ? 1 : 0; Base != 256; ++Base)
      for (unsigned Route : {0u, 1u, 2u}) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X86));
        const uint8_t Bytes[]{0x66, 0x67, 0x65, Opcode, uint8_t(Base)};
        DecodedInsn I{};
        const int Size =
            Route == 0   ? D.decodeOne(Bytes, sizeof(Bytes), kEntry, I)
            : Route == 1 ? D.decodeOneLight(Bytes, sizeof(Bytes), kEntry, I)
                         : D.decodeOneForLift(Bytes, sizeof(Bytes), kEntry, I);
        ASSERT_EQ(Size, sizeof(Bytes));
        std::vector<LowOp> Ops;
        LowInstructionUndefinedEffects Effects;
        D.liftToLow(I, Ops, {}, {}, &Effects);
        ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
            << Effects.Diagnostic;
        BinaryImage Image;
        Image.Arch = Arch::X86;
        Image.Bits = Bitness::Bits32;
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        for (unsigned AX :
             {0u, 1u, 0xffu, 0x100u, 0x1234u, 0x7fffu, 0x8000u, 0xffffu}) {
          E.reset();
          E.setRegister(x86reg::RAX, AX & 255);
          E.setRegister(x86reg::RAX + 1, AX >> 8);
          ASSERT_EQ(E.run(Ops), Ops.size());
          EXPECT_EQ(E.getRegister(x86reg::RAX),
                    Opcode == 0xd4 ? (AX & 255) % Base
                                   : ((AX & 255) + (AX >> 8) * Base) % 256);
          EXPECT_EQ(E.getRegister(x86reg::RAX + 1),
                    Opcode == 0xd4 ? (AX & 255) / Base : 0);
          ++Cases;
        }
      }
  RecordProperty("value_cases", Cases);
}

TEST(X86DecimalAdjust, AuditsExactlyTheArchitecturallyUndefinedBits) {
  const unsigned Offsets[]{x86reg::CF, x86reg::PF, x86reg::AF,
                           x86reg::ZF, x86reg::SF, x86reg::OF};
  const unsigned Bits[]{0, 2, 4, 6, 7, 11};
  for (uint8_t Opcode : {0x27, 0x2f, 0x37, 0x3f, 0xd4, 0xd5})
    for (bool Strict : {false, true})
      for (uint8_t Segment : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65}) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X86));
        D.setStrict(Strict);
        std::vector<uint8_t> Bytes{0x66, 0x67};
        if (Segment)
          Bytes.push_back(Segment);
        Bytes.push_back(Opcode);
        if (Opcode >= 0xd4)
          Bytes.push_back(10);
        DecodedInsn I{};
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, I),
                  Bytes.size());
        std::vector<LowOp> Ops;
        LowInstructionUndefinedEffects Effects;
        D.liftToLow(I, Ops, {}, {}, &Effects);
        if (!Strict) {
          EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
          EXPECT_TRUE(Effects.Effects.empty());
          continue;
        }
        ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
            << Effects.Diagnostic;
        EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
        const unsigned Mask = Opcode < 0x30   ? 0x800
                              : Opcode < 0x40 ? 0x8c4
                                              : 0x811;
        unsigned Index = 0;
        for (unsigned B = 0; B != 6; ++B) {
          if (!(Mask & (1u << Bits[B])))
            continue;
          ASSERT_LT(Index, Effects.Effects.size());
          const auto &E = Effects.Effects[Index++];
          EXPECT_EQ(E.Output, NdVar::reg(Offsets[B], 1));
          EXPECT_EQ(E.BitOffset, 0u);
          EXPECT_EQ(E.BitCount, 1u);
          EXPECT_FALSE(E.When);
          EXPECT_EQ(E.AfterOp, Ops.size());
        }
        EXPECT_EQ(Index, Effects.Effects.size());
      }
}

TEST(X86DecimalAdjust, OriginalBytesOwnTheBaseAndRejectUnownedForms) {
  for (bool Strict : {false, true}) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X86));
    D.setStrict(Strict);
    for (uint8_t Opcode : {0xd4, 0xd5}) {
      const uint8_t Bytes[]{Opcode, 0};
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), kEntry, I), 2);
      I.Raw->detail->x86.op_count = 1;
      I.Raw->detail->x86.operands[0].type = X86_OP_IMM;
      I.Raw->detail->x86.operands[0].imm = 1;
      std::vector<LowOp> Ops;
      if (Opcode == 0xd4) {
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      } else {
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        for (const auto &Op : Ops)
          if (Op.Opcode == NdOp::INT_MULT) {
            EXPECT_TRUE(Op.Inputs[1].isConst());
            EXPECT_EQ(Op.Inputs[1].Offset, 0u);
          }
      }
    }
    for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
             {0xf3, 0x27}, {0xf2, 0x37}, {0x66, 0x66, 0xd5, 10}}) {
      DecodedInsn I{};
      if (D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, I) <= 0)
        continue;
      std::vector<LowOp> Ops;
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}
