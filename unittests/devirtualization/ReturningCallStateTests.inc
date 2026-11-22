//===- ReturningCallStateTests.inc - Conditional return state tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently specified byte observations of the production call consumer.
/// Included after the scalar/runtime-control fixture helpers.
//===----------------------------------------------------------------------===//

class ReturningCallProvider : public Provider {
  std::shared_ptr<const SpecializationRawEnvironment> Domain;

public:
  std::shared_ptr<const SpecializationRawEnvironment>
  rawEnvironment() const override {
    return Domain;
  }

  llvm::Error prepare(const SpecializationOptions &O) {
    auto Prepared = prepareRawEnvironment(O, 0, 0x10000, std::string(64, 'A'));
    if (!Prepared)
      return Prepared.takeError();
    Domain = std::move(*Prepared);
    return llvm::Error::success();
  }

  void native(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    D.setStrict(true);
    DecodedInsn Decoded{};
    ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Decoded),
              Bytes.size());
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    D.liftToLow(Decoded, Ops, {}, {}, &Effects);
    add(Address, std::move(Ops), Address + Bytes.size());
    auto &I = Code.at(Address);
    I.Origin.Size = Bytes.size();
    I.NativeBytes.assign(Bytes.begin(), Bytes.end());
    I.UndefinedEffects = std::move(Effects);
    if (Decoded.Id == X86_INS_CALL) {
      I.IsNativeCall = true;
      I.NativeStackControl = SpecializationNativeStackControl::Call;
      I.Origin.Control = LowInstructionControl::Call;
      I.Origin.ControlFlags = LowInstructionControlFlag::Call;
      if (I.Ops.back().Opcode == NdOp::INDIR_CALL)
        I.Origin.ControlFlags |= LowInstructionControlFlag::Indirect;
      else
        I.Origin.Immediate = I.Ops.back().Inputs[0].Offset;
    } else if (Decoded.Id == X86_INS_RET) {
      I.NativeStackControl = SpecializationNativeStackControl::Return;
      I.Origin.Control = LowInstructionControl::Return;
      I.Origin.ControlFlags = LowInstructionControlFlag::Return;
      I.Origin.Immediate = D.returnImmediate(Decoded);
    }
  }
};

SpecializationOptions returningStateOptions(va_t Call = 0x101,
                                            va_t Exit = 0x104) {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-128, 256};
  O.X64MachineExits = {{1, Exit, SpecializationX64MachineExitKind::Return}};
  O.X64ReturningCalls = {
      {1, Call, SpecializationX64CallProfile::ConditionalNormalReturnV1, 32}};
  // This fixture observes every byte of the vector/tile banks, not just the
  // scalar subset normally selected for control recovery.
  O.MaxOperations = 2'000'000;
  O.MaxSymbolicNodes = 262'144;
  O.MaxRawEnvironmentResultBytes = 128 * 1024 * 1024;
  return O;
}

uint8_t returningStateCanary(uint64_t Byte, unsigned Seed) {
  if (Byte >= x86reg::CF && Byte <= x86reg::DF)
    return (Byte ^ Seed) & 1;
  return Seed == 0 ? static_cast<uint8_t>(Byte * 37 + 0xa3)
                   : static_cast<uint8_t>((Byte >> 8) * 97 + 0x59);
}

// Deliberately independent of TargetRegInfo/returningCallTransition and the
// symbolic preservation primitive. These are the declared V1 byte equations.
bool returningStatePreservedByte(uint64_t Byte) {
  const uint64_t GPRs[] = {x86reg::RBX, x86reg::RBP, x86reg::RSI, x86reg::RDI,
                           x86reg::R12, x86reg::R13, x86reg::R14, x86reg::R15,
                           x86reg::R30, x86reg::R31};
  for (uint64_t R : GPRs)
    if (Byte >= R && Byte - R < 8)
      return true;
  for (unsigned R = 6; R != 16; ++R)
    if (Byte >= x86reg::vectorReg(R) && Byte - x86reg::vectorReg(R) < 16)
      return true;
  return Byte == x86reg::DF ||
         (Byte >= x86reg::FPU_CW && Byte - x86reg::FPU_CW < 2);
}

std::map<uint64_t, NdVar>
returningStateObservations(const SpecializationResult &R, uint64_t Begin,
                           uint64_t End) {
  std::map<uint64_t, NdVar> Values;
  for (const auto &B : R.Residual.Blocks)
    for (const auto &Op : B.Ops)
      if (Op.Opcode == NdOp::COPY && Op.Output.isTemp() &&
          Op.Output.Offset >= Begin && Op.Output.Offset < End)
        EXPECT_TRUE(Values.emplace(Op.Output.Offset, Op.Inputs[0]).second);
  return Values;
}

TEST(InterpreterSpecialization, ReturningCallAllRegisterBytesMatchStateModel) {
  for (unsigned Seed : {0U, 1U}) {
    SCOPED_TRACE(Seed);
    ReturningCallProvider P;
    constexpr uint64_t Sink = 0x20000;
    std::vector<LowOp> Before, After;
    std::vector<uint64_t> Bytes;
    for (uint64_t Byte = 0; Byte < x86reg::TileConfig + x86reg::TileConfigSize;
         ++Byte) {
      if (Byte >= x86reg::RSP && Byte - x86reg::RSP < 8)
        continue;
      // Include unused gaps too: the relation promises a fresh default for
      // every register location outside its finite preserved/assigned ranges.
      Bytes.push_back(Byte);
      Before.push_back(op(NdOp::COPY, reg(Byte, 1),
                          {constant(returningStateCanary(Byte, Seed), 1)}));
      After.push_back(
          op(NdOp::COPY, NdVar::tmp(Sink + Byte, 1), {reg(Byte, 1)}));
    }
    P.add(0x100, std::move(Before));
    P.native(0x101, {0xe8, 0x00, 0x01, 0x00, 0x00});
    P.add(0x106, std::move(After));
    P.native(0x107, {0xc3});
    const auto O = returningStateOptions(0x101, 0x107);
    auto Prepared = P.prepare(O);
    ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(R.ReturningCalls.size(), 1U);
    const auto Actual = returningStateObservations(R, Sink, Sink + 0x10000);
    ASSERT_EQ(Actual.size(), Bytes.size());
    for (uint64_t Byte : Bytes) {
      SCOPED_TRACE(Byte);
      const auto Value = Actual.at(Sink + Byte);
      const bool PC = Byte >= x86reg::RIP && Byte - x86reg::RIP < 8;
      EXPECT_EQ(Value.isConst(), PC || returningStatePreservedByte(Byte));
      if (Value.isConst())
        EXPECT_EQ(Value.Offset,
                  PC ? uint8_t(uint64_t{0x106} >> (8 * (Byte - x86reg::RIP)))
                     : returningStateCanary(Byte, Seed));
    }
  }
}

TEST(InterpreterSpecialization,
     ReturningCallDefinesSixIndependentBooleanFlags) {
  ReturningCallProvider P;
  std::vector<LowOp> Before, After;
  const uint64_t Flags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                            x86reg::ZF, x86reg::SF, x86reg::OF};
  for (uint64_t Flag : Flags) {
    Before.push_back(op(NdOp::COPY, reg(Flag, 1), {constant(0xa5, 1)}));
    After.push_back(op(NdOp::INT_AND, NdVar::tmp(0x20000 + Flag, 1),
                       {reg(Flag, 1), constant(0xfe, 1)}));
    After.push_back(op(NdOp::COPY, NdVar::tmp(0x30000 + Flag, 1),
                       {NdVar::tmp(0x20000 + Flag, 1)}));
  }
  P.add(0x100, std::move(Before));
  P.native(0x101, {0xff, 0xd7});
  P.add(0x103, std::move(After));
  P.native(0x104, {0xc3});
  const auto O = returningStateOptions();
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Actual = returningStateObservations(R, 0x30000, 0x40000);
  ASSERT_EQ(Actual.size(), std::size(Flags));
  for (uint64_t Flag : Flags) {
    EXPECT_TRUE(Actual.at(0x30000 + Flag).isConst());
    EXPECT_EQ(Actual.at(0x30000 + Flag).Offset, 0U);
  }
  // Each pair can agree and disagree. A reused flag sample wrongly eliminates
  // the inequality path. Unwritten and produced values have a separate test.
  for (size_t I = 0; I < std::size(Flags); ++I)
    for (size_t J = I + 1; J < std::size(Flags); ++J) {
      P.add(0x103,
            {op(NdOp::INT_EQUAL, NdVar::tmp(0x20000, 1),
                {reg(Flags[I], 1), reg(Flags[J], 1)}),
             op(NdOp::COND_BR, {}, {constant(0x105), NdVar::tmp(0x20000, 1)})},
            0x104);
      P.native(0x105, {0xc3});
      auto Both = O;
      Both.X64MachineExits.push_back(
          {1, 0x105, SpecializationX64MachineExitKind::Return});
      auto PreparedBoth = P.prepare(Both);
      ASSERT_FALSE(bool(PreparedBoth))
          << llvm::toString(std::move(PreparedBoth));
      const auto Result = specializeInterpreter(P, {0x100}, Both);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(Result.ExternalExits.size(), 2U) << I << ',' << J;
    }
}

TEST(InterpreterSpecialization,
     ReturningCallReplacesUnwrittenAndProducedFlagValues) {
  for (bool NativeXor : {false, true}) {
    SCOPED_TRACE(NativeXor);
    ReturningCallProvider P;
    if (NativeXor) {
      P.native(0x100, {0x31, 0xc0}); // XOR EAX,EAX produces arbitrary AF.
      const auto &Effects = P.Code.at(0x100).UndefinedEffects;
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
      ASSERT_TRUE(std::any_of(
          Effects.Effects.begin(), Effects.Effects.end(), [](const auto &E) {
            return E.Output == reg(x86reg::AF, 1) && E.BitOffset == 0 &&
                   E.BitCount == 1 && !E.When;
          }));
    }
    P.native(0x102, {0xff, 0xd7});
    P.add(0x104,
          {op(NdOp::INT_AND, NdVar::tmp(0x2000, 1),
              {reg(x86reg::AF, 1), constant(0xfe, 1)}),
           op(NdOp::COPY, NdVar::tmp(0x3000, 1), {NdVar::tmp(0x2000, 1)}),
           op(NdOp::COND_BR, {}, {constant(0x106), reg(x86reg::AF, 1)})},
          0x105);
    P.native(0x105, {0xc3});
    P.native(0x106, {0xc3});
    auto O = returningStateOptions(0x102, 0x105);
    O.X64MachineExits.push_back(
        {1, 0x106, SpecializationX64MachineExitKind::Return});
    auto Prepared = P.prepare(O);
    ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
    const auto R = specializeInterpreter(P, {NativeXor ? 0x100U : 0x102U}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(R.ReturningCalls.size(), 1U);
    EXPECT_EQ(R.ExternalExits.size(), 2U);
    const auto Actual = returningStateObservations(R, 0x3000, 0x3001);
    ASSERT_EQ(Actual.size(), 1U);
    const auto Value = Actual.at(0x3000);
    ASSERT_TRUE(Value.isConst());
    EXPECT_EQ(Value.Size, 1U);
    EXPECT_EQ(Value.Offset, 0U);
    // Returning-call admission requires ExplicitMachineState. Its entry
    // flags are bound, so the ordinary-ABI UndefinedFlags sidecar is empty.
    // These checks cover replacement of an unwritten input or XOR's selected
    // implementation value, not an unreachable sidecar state or ISA proof.
  }
}

TEST(InterpreterSpecialization, ReturningCallPreservesCallerStackAndControls) {
  for (uint32_t Mask : {0xffbfU, 0xffffU}) {
    ReturningCallProvider P;
    P.add(0x100, {op(NdOp::COPY, reg(x86reg::RBX), {reg(x86reg::RSP)}),
                  mxcsrWrite(constant(Mask, 4)), writePriority(constant(13))});
    P.native(0x101, {0xff, 0xd7});
    P.add(0x103,
          {op(NdOp::INT_SUB, NdVar::tmp(0x20000, 8),
              {reg(x86reg::RSP), reg(x86reg::RBX)}),
           op(NdOp::COPY, NdVar::tmp(0x30000, 8), {NdVar::tmp(0x20000, 8)}),
           mxcsrRead(NdVar::tmp(0x20008, 4)),
           op(NdOp::COPY, NdVar::tmp(0x30008, 4), {NdVar::tmp(0x20008, 4)}),
           readPriority(NdVar::tmp(0x20010, 8)),
           op(NdOp::COPY, NdVar::tmp(0x30010, 8), {NdVar::tmp(0x20010, 8)})});
    P.native(0x104, {0xc3});
    auto O = returningStateOptions();
    O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
    O.X64MXCSRMask = Mask;
    O.X64CR8State = true;
    auto Prepared = P.prepare(O);
    ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    const auto Actual = returningStateObservations(R, 0x30000, 0x40000);
    ASSERT_EQ(Actual.size(), 3U);
    for (const auto &[Offset, Expected] : std::map<uint64_t, uint64_t>{
             {0x30000, 0}, {0x30008, Mask}, {0x30010, 13}}) {
      EXPECT_TRUE(Actual.at(Offset).isConst());
      EXPECT_EQ(Actual.at(Offset).Offset, Expected);
    }
  }
}

TEST(InterpreterSpecialization,
     ReturningCallUnreadBytesKeepOnlyPromisedIdentity) {
  const uint64_t Bytes[] = {x86reg::RAX,
                            x86reg::R8 + 7,
                            x86reg::RBX + 7,
                            x86reg::RBP,
                            x86reg::RDI + 3,
                            x86reg::R12,
                            x86reg::R29 + 7,
                            x86reg::R30 + 7,
                            x86reg::R31,
                            x86reg::XMM0,
                            x86reg::XMM6 + 15,
                            x86reg::XMM6 + 16,
                            x86reg::XMM15 + 63,
                            x86reg::XMM31 + 63,
                            x86reg::K0,
                            x86reg::ST0,
                            x86reg::FPU_SW,
                            x86reg::FPU_CW + 1,
                            x86reg::DF,
                            x86reg::TMM0,
                            x86reg::TileConfig + 63,
                            192};
  for (uint64_t Byte : Bytes) {
    SCOPED_TRACE(Byte);
    ReturningCallProvider P;
    // Save one initially unknown byte in the actual protected frame suffix.
    // No entry constant may conceal retention of an old symbolic identity.
    P.add(0x100, {op(NdOp::INT_ADD, NdVar::tmp(0x1000, 8),
                     {reg(x86reg::RSP), constant(40)}),
                  op(NdOp::STORE, {}, {NdVar::tmp(0x1000, 8), reg(Byte, 1)})});
    P.native(0x101, {0xff, 0xd7});
    P.add(0x103,
          {op(NdOp::INT_ADD, NdVar::tmp(0x1000, 8),
              {reg(x86reg::RSP), constant(40)}),
           op(NdOp::LOAD, NdVar::tmp(0x1008, 1), {NdVar::tmp(0x1000, 8)}),
           op(NdOp::INT_EQUAL, NdVar::tmp(0x1010, 1),
              {reg(Byte, 1), NdVar::tmp(0x1008, 1)}),
           op(NdOp::COND_BR, {}, {constant(0x105), NdVar::tmp(0x1010, 1)})},
          0x104);
    P.native(0x104, {0xc3});
    P.native(0x105, {0xc3});
    auto O = returningStateOptions();
    O.X64MachineExits.push_back(
        {1, 0x105, SpecializationX64MachineExitKind::Return});
    auto Prepared = P.prepare(O);
    ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(R.ExternalExits.size(),
              returningStatePreservedByte(Byte) ? 1U : 2U);
    if (returningStatePreservedByte(Byte))
      EXPECT_EQ(R.ExternalExits.front().Original.Origin.Address, 0x105U);
  }
}

TEST(InterpreterSpecialization, ReturningCallCurrentMemoryMatchesByteModel) {
  ReturningCallProvider P;
  std::map<int64_t, uint8_t> Current;
  std::vector<LowOp> Before, After;
  const auto Address = NdVar::tmp(0x1000, 8);
  // The caller moves RSP before the invocation. The protected suffix starts
  // at entryRSP-16, not at the outgoing extent relative to entryRSP.
  Before.push_back(
      op(NdOp::INT_SUB, reg(x86reg::RSP), {reg(x86reg::RSP), constant(64)}));
  for (int64_t Byte = -128; Byte != 256; ++Byte) {
    if (Byte % 11 == 0)
      continue; // absent bytes remain holes, including in the kept suffix
    Current[Byte] = static_cast<uint8_t>(Byte * 43 + 0x52);
    Before.push_back(
        op(NdOp::INT_ADD, Address, {reg(x86reg::RSP), constant(Byte + 64)}));
    Before.push_back(
        op(NdOp::STORE, {}, {Address, constant(Current[Byte], 1)}));
  }
  // Partial overwrites must keep the current byte, not an earlier snapshot.
  for (int64_t Byte : {-16, 17, 254}) {
    Current[Byte] = 0xe7;
    Before.push_back(
        op(NdOp::INT_ADD, Address, {reg(x86reg::RSP), constant(Byte + 64)}));
    Before.push_back(op(NdOp::STORE, {}, {Address, constant(0xe7, 1)}));
  }
  for (int64_t Byte = -128; Byte != 256; ++Byte) {
    const auto Value = NdVar::tmp(0x2000 + Byte + 128, 1);
    After.push_back(
        op(NdOp::INT_ADD, Address, {reg(x86reg::RSP), constant(Byte + 64)}));
    After.push_back(op(NdOp::LOAD, Value, {Address}));
    After.push_back(
        op(NdOp::COPY, NdVar::tmp(0x3000 + Byte + 128, 1), {Value}));
  }
  P.add(0x100, std::move(Before));
  P.native(0x101, {0xff, 0xd7});
  P.add(0x103, std::move(After));
  P.native(0x104, {0xc3});
  auto O = returningStateOptions();
  O.X64ReturningCalls[0].OutgoingBytes = 48;
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Actual = returningStateObservations(R, 0x3000, 0x4000);
  ASSERT_EQ(Actual.size(), 384U);
  for (int64_t Byte = -128; Byte != 256; ++Byte) {
    SCOPED_TRACE(Byte);
    const auto Value = Actual.at(0x3000 + Byte + 128);
    const bool Kept = Byte >= -16 && Current.contains(Byte);
    EXPECT_EQ(Value.isConst(), Kept);
    if (Kept && Value.isConst())
      EXPECT_EQ(Value.Offset, Current.at(Byte));
  }
}

TEST(InterpreterSpecialization,
     ReturningCallFreshBytesAreIndependentAndStable) {
  const std::pair<NdVar, NdVar> Pairs[] = {
      {reg(x86reg::RAX), reg(x86reg::R8)},
      {reg(x86reg::RAX, 1), reg(x86reg::RAX + 1, 1)},
      {reg(x86reg::XMM0, 1), reg(x86reg::XMM0 + 63, 1)},
      {reg(x86reg::XMM6 + 16, 1), reg(x86reg::XMM31 + 63, 1)},
      {reg(x86reg::K0, 1), reg(x86reg::FPU_SW, 1)},
      {reg(x86reg::TMM0, 1), reg(x86reg::TileConfig + 63, 1)},
      {reg(x86reg::R29, 8), reg(x86reg::RAX, 8)}};
  for (const auto &[Left, Other] : Pairs)
    for (bool Same : {false, true}) {
      ReturningCallProvider P;
      P.native(0x101, {0xff, 0xd7});
      P.add(0x103,
            {op(NdOp::COPY, NdVar::tmp(0x1000, Left.Size), {Left}),
             op(NdOp::INT_EQUAL, NdVar::tmp(0x1010, 1),
                {NdVar::tmp(0x1000, Left.Size), Same ? Left : Other}),
             op(NdOp::COND_BR, {}, {constant(0x105), NdVar::tmp(0x1010, 1)})},
            0x104);
      P.native(0x104, {0xc3});
      P.native(0x105, {0xc3});
      auto O = returningStateOptions();
      O.X64MachineExits.push_back(
          {1, 0x105, SpecializationX64MachineExitKind::Return});
      auto Prepared = P.prepare(O);
      ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
      const auto R = specializeInterpreter(P, {0x101}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_EQ(R.ExternalExits.size(), Same ? 1U : 2U);
      if (Same)
        EXPECT_EQ(R.ExternalExits.front().Original.Origin.Address, 0x105U);
    }
}
