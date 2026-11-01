//===- MedScalarSelectRangeTests.inc - Guarded index bounds -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check machine-width guard correlations without changing executable IR.
///
//===----------------------------------------------------------------------===//

namespace {

MedVar scalarTemp(int Id, uint16_t Bytes, Arch Architecture) {
  MedVar Value = temp(Id, Architecture);
  Value.Size = Bytes;
  return Value;
}

/// Model x64 CMOV's actual low-dword comparison and byte-register transport.
/// The chosen index is (uint32_t)(value - First) <= Span ? (uint8_t)value : 2.
MedFunc guardedByteIndex(Arch Architecture, uint32_t First = 1,
                         uint32_t Span = 1, bool Masked = true,
                         bool Reverse = false) {
  auto Func = frameWithEntrySP(Architecture);
  auto &Ops = Func.Blocks[0].Ops;
  const auto T = [&](int Id, uint16_t Bytes) {
    return scalarTemp(Id, Bytes, Architecture);
  };
  const auto C = [&](uint64_t Value, uint16_t Bytes) {
    return MedVar::makeConst(Value, Bytes, ConstantAddressProvenance::Scalar);
  };
  const auto Width = getTargetRegInfo(Architecture).PointerSize;
  const uint64_t NegativeFirst =
      Width == 8 ? uint64_t{0} - First : uint32_t{0} - First;
  Ops.push_back(op(NdOp::LOAD, T(2, 4), C(0x8000, Width)));
  Ops.push_back(
      op(Width == 8 ? NdOp::INT_ZEXT : NdOp::COPY, T(3, Width), T(2, 4)));
  Ops.push_back(op(NdOp::SUBBYTES, T(4, 1), T(3, Width), C(0, Width)));
  Ops.push_back(op(NdOp::INT_ZEXT, T(5, 4), T(4, 1)));
  Ops.push_back(
      op(NdOp::INT_ADD, T(6, Width), T(3, Width), C(NegativeFirst, Width)));
  Ops.push_back(op(NdOp::SUBBYTES, T(7, 4), T(6, Width), C(0, Width)));
  Ops.push_back(
      op(Width == 8 ? NdOp::INT_ZEXT : NdOp::COPY, T(8, Width), T(7, 4)));
  Ops.push_back(op(NdOp::SUBBYTES, T(9, 4), T(8, Width), C(0, Width)));
  Ops.push_back(op(NdOp::INT_LESS, T(10, 1), T(9, 4), C(Span, 4)));
  Ops.push_back(op(NdOp::INT_SUB, T(11, 4), T(9, 4), C(Span, 4)));
  Ops.push_back(op(NdOp::INT_EQUAL, T(12, 1), T(11, 4), C(0, 4)));
  Ops.push_back(op(NdOp::BOOL_OR, T(13, 1), T(10, 1), T(12, 1)));
  Ops.push_back(op(NdOp::INT_ZEXT, T(14, 4), T(13, 1)));
  Ops.push_back(op(NdOp::INT_NEG2, T(15, 4), T(14, 4)));
  Ops.push_back(op(NdOp::INT_NOT, T(16, 4), T(15, 4)));
  Ops.push_back(op(NdOp::INT_AND, T(17, 4), T(5, 4), T(15, 4)));
  Ops.push_back(op(NdOp::CONCAT, T(18, Width), T(99, Width - 1), C(2, 1)));
  Ops.push_back(op(NdOp::COPY, T(19, Width), T(18, Width)));
  Ops.push_back(op(NdOp::SUBBYTES, T(20, 1), T(19, Width), C(0, Width)));
  Ops.push_back(op(NdOp::INT_ZEXT, T(21, 4), T(20, 1)));
  Ops.push_back(op(NdOp::INT_AND, T(22, 4), T(21, 4), T(16, 4)));
  Ops.push_back(op(NdOp::INT_OR, T(23, 4), T(17, 4), T(22, 4)));
  if (!Masked) {
    Ops.back() = op(NdOp::SELECT, T(23, 4), T(13, 1), T(5, 4));
    Ops.back().addInput(T(21, 4));
  }
  if (Reverse) {
    for (auto &Op : Ops)
      if (Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
          Op.Opcode == NdOp::BOOL_OR || Op.Opcode == NdOp::INT_EQUAL)
        std::swap(Op.Inputs[0], Op.Inputs[1]);
  }
  Ops.push_back(op(NdOp::SUBBYTES, T(24, 1), T(23, 4), C(0, Width)));
  Ops.push_back(op(NdOp::INT_ZEXT, T(25, Width), T(24, 1)));
  Ops.push_back(op(NdOp::INT_MULT, T(26, Width), T(25, Width), C(4, Width)));
  Ops.push_back(
      op(NdOp::INT_ADD, T(27, Width), reg(0, Architecture), T(26, Width)));
  return Func;
}

TEST(MedStackAlignment, GuardedByteIndicesKeepTheComparisonCorrelation) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Masked : {false, true})
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(int(Architecture));
          SCOPED_TRACE(int(Format));
          SCOPED_TRACE(Masked);
          SCOPED_TRACE(Reverse);
          auto Func = guardedByteIndex(Architecture, 1, 1, Masked, Reverse);
          const auto Address = Func.Blocks[0].Ops.back().Output;
          const auto Ranges =
              entryStackOffsetRanges(Func, {Address}, Architecture, Format);
          ASSERT_TRUE(Ranges[0]);
          EXPECT_EQ(Ranges[0]->Min, 4);
          EXPECT_EQ(Ranges[0]->Max, 8);
          EXPECT_FALSE(entryStackOffset(Func, Address, Architecture, Format));
          // Queries carry no assumptions into a later independent query.
          const auto Unconstrained = scalarTemp(
              3, getTargetRegInfo(Architecture).PointerSize, Architecture);
          EXPECT_FALSE(entryStackOffsetRanges(Func, {Unconstrained},
                                              Architecture, Format)[0]);
        }
}

TEST(MedStackAlignment, GuardedIndicesRetainModularWrapAndByteTruncation) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (uint32_t First :
         {0u, 1u, 2u, 127u, 254u, 255u, 256u, UINT32_MAX - 2, UINT32_MAX})
      for (uint32_t Span : {0u, 1u, 2u, 7u}) {
        SCOPED_TRACE(int(Architecture));
        SCOPED_TRACE(First);
        SCOPED_TRACE(Span);
        auto Func = guardedByteIndex(Architecture, First, Span);
        const auto Address = Func.Blocks[0].Ops.back().Output;
        const auto Ranges = entryStackOffsetRanges(
            Func, {Address}, Architecture, BinaryFormat::COFF);
        ASSERT_TRUE(Ranges[0]);
        // Enumerate every accepted full-width value and both adjacent rejected
        // values, including the uint32_t wrap. No symbolic evaluator supplies
        // the expected result.
        for (uint32_t Delta = 0; Delta <= Span + 2; ++Delta) {
          const uint32_t Input = First + Delta - 1;
          const uint32_t Index =
              uint32_t(Input - First) <= Span ? uint8_t(Input) : 2;
          EXPECT_LE(Ranges[0]->Min, int64_t(Index) * 4);
          EXPECT_GE(Ranges[0]->Max, int64_t(Index) * 4);
        }
        EXPECT_LE(Ranges[0]->Min, 8);
        EXPECT_GE(Ranges[0]->Max, 8);
      }
}

TEST(MedStackAlignment, StaleOrNonBooleanSelectsCannotEstablishNarrowBounds) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Func = guardedByteIndex(Arch::X64);
    auto &Ops = Func.Blocks[0].Ops;
    switch (Mutation) {
    case 0:
      Ops[9].Inputs[1].Provenance = ConstantAddressProvenance::DataAddress;
      break;
    case 1:
      Ops[12].Opcode = NdOp::INT_OR;
      break;
    case 2:
      Ops[14].Opcode = NdOp::COPY;
      break;
    case 3:
      Ops[15].Opcode = NdOp::COPY;
      break;
    case 4:
      Ops[21].Inputs[1] = Ops[16].Inputs[1];
      break;
    case 5:
      Ops[15].Inputs[0].SSAVer += 1;
      break;
    case 6:
      Ops[12].Output.Size = 2;
      break;
    case 7:
      Ops[9].Inputs[0].Size = 8;
      break;
    case 8:
      Ops[6].Inputs[1].ConstVal = 1;
      break;
    case 9:
      Ops[4].Inputs[0] = scalarTemp(100, 1, Arch::X64);
      break;
    case 10:
      Ops[12].Dead = true;
      break;
    case 11:
      Ops[12].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 12:
      Ops[12].MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 13:
      Ops.push_back(Ops[12]);
      break;
    case 14:
      Ops[13].Inputs[0] = scalarTemp(101, 1, Arch::X64);
      break;
    case 15:
      Ops[14].Inputs[0].Size = 2;
      break;
    case 16:
      Ops.push_back(Ops[1]);
      break;
    case 17:
      Ops[5].Inputs[0] = Ops[5].Output;
      break;
    }
    const auto Address = scalarTemp(27, 8, Arch::X64);
    const auto Ranges =
        entryStackOffsetRanges(Func, {Address}, Arch::X64, BinaryFormat::COFF);
    EXPECT_TRUE(!Ranges[0] || Ranges[0]->Min < 4 || Ranges[0]->Max > 8);
  }
}

TEST(MedStackAlignment, GuardedIndexRelationsDoNotEscapeTheirSelectedArm) {
  auto Func = guardedByteIndex(Arch::X64);
  auto &Ops = Func.Blocks[0].Ops;
  // Choosing the raw byte on the false arm has a counterexample at 255.
  std::swap(Ops[16].Inputs[0], Ops[21].Inputs[0]);
  const auto Address = Ops.back().Output;
  auto Ranges =
      entryStackOffsetRanges(Func, {Address}, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(Ranges[0]);
  EXPECT_GE(Ranges[0]->Max, 1020);
  // A changed SSA occurrence of the comparison input is independent of the
  // selected value even if it has the same register and width.
  Func = guardedByteIndex(Arch::X64);
  Func.Blocks[0].Ops[5].Inputs[0].SSAVer += 1;
  Ranges =
      entryStackOffsetRanges(Func, {Address}, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(Ranges[0]);
  EXPECT_GE(Ranges[0]->Max, 1020);
  Func = guardedByteIndex(Arch::X64);
  const auto RawAddress = scalarTemp(28, 8, Arch::X64);
  Func.Blocks[0].Ops.push_back(op(NdOp::INT_ADD, RawAddress, reg(0, Arch::X64),
                                  scalarTemp(3, 8, Arch::X64)));
  Ranges = entryStackOffsetRanges(Func, {Address, RawAddress}, Arch::X64,
                                  BinaryFormat::COFF);
  ASSERT_TRUE(Ranges[0]);
  ASSERT_TRUE(Ranges[1]);
  EXPECT_EQ(Ranges[0]->Max, 8);
  EXPECT_EQ(Ranges[1]->Min, 0);
  EXPECT_EQ(Ranges[1]->Max, int64_t{UINT32_MAX});
}

TEST(MedStackAlignment, InvertedAndSignedConditionsKeepTheirOwnDomains) {
  auto Func = guardedByteIndex(Arch::X64);
  auto &Ops = Func.Blocks[0].Ops;
  const auto Condition = scalarTemp(40, 1, Arch::X64);
  Ops.insert(Ops.begin() + 13,
             op(NdOp::BOOL_NOT, Condition, scalarTemp(13, 1, Arch::X64)));
  Ops[14].Inputs[0] = Condition;
  std::swap(Ops[17].Inputs[0], Ops[22].Inputs[0]);
  const auto Address = Ops.back().Output;
  auto Ranges =
      entryStackOffsetRanges(Func, {Address}, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(Ranges[0]);
  EXPECT_EQ(Ranges[0]->Min, 4);
  EXPECT_EQ(Ranges[0]->Max, 8);
  Func = guardedByteIndex(Arch::X64);
  Func.Blocks[0].Ops[9].Opcode = NdOp::INT_SLESS;
  Ranges =
      entryStackOffsetRanges(Func, {Address}, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(Ranges[0]);
  // -1 is accepted by the signed comparison and contributes byte 255.
  EXPECT_GE(Ranges[0]->Max, 1020);
}
} // namespace
