//===- SupervisorReadRecoveryTests.inc - Supervisor runtime reads -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Supervisor runtime-read transitions, preserved state and proof boundaries.
///
//===----------------------------------------------------------------------===//

void addMSRRead(Provider &P, va_t Address) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  const uint8_t Bytes[]{0x0f, 0x32};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Address, I),
            sizeof(Bytes));
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  D.liftToLow(I, Ops, {}, {}, &Effects);
  P.add(Address, std::move(Ops), Address + sizeof(Bytes));
  auto &Instruction = P.Code.at(Address);
  Instruction.Origin.Size = sizeof(Bytes);
  Instruction.NativeBytes.assign(std::begin(Bytes), std::end(Bytes));
  Instruction.UndefinedEffects = std::move(Effects);
}

SpecializationOptions supervisorReadOptions() {
  auto O = wideOptions();
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  return O;
}

std::optional<uint64_t>
executeMSRReads(const LowFunc &F, std::map<uint64_t, uint64_t> Inputs,
                llvm::ArrayRef<MSRReadObservation> Reads) {
  return execute(F, std::move(Inputs), {}, 0, {}, std::nullopt, {}, nullptr,
                 std::nullopt, 0xffbf, nullptr, {}, Reads);
}

TEST(InterpreterSpecialization,
     SupervisorReadRetainsSelectorAndCompleteSample) {
  Provider P;
  addMSRRead(P, 0x100);
  P.add(0x102,
        {op(NdOp::INT_LEFT, reg(x86reg::RDX), {reg(x86reg::RDX), constant(32)}),
         op(NdOp::INT_OR, reg(0), {reg(0), reg(x86reg::RDX)}),
         op(NdOp::INT_XOR, reg(0), {reg(0), reg(x86reg::RCX)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, supervisorReadOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 1u);
  for (uint32_t Index : {0x10u, 0x1bu, 0xc0000080u, 0xc0000082u})
    for (uint64_t Value :
         {UINT64_C(0), UINT64_C(0xffffffff), UINT64_C(0x8000000000000000),
          UINT64_C(0xfedcba9876543210), UINT64_MAX}) {
      const uint64_t Selector = UINT64_C(0x1234567800000000) | Index;
      EXPECT_EQ(executeMSRReads(R.Residual,
                                {{x86reg::RCX, Selector},
                                 {x86reg::RAX, UINT64_MAX},
                                 {x86reg::RDX, UINT64_MAX}},
                                {{Index, Value}}),
                Value ^ Selector);
    }
}

TEST(InterpreterSpecialization, SupervisorReadsRemainIndependent) {
  Provider P;
  addMSRRead(P, 0x100);
  P.add(0x102, {op(NdOp::COPY, reg(80), {reg(0)})}, 0x110);
  addMSRRead(P, 0x110);
  P.add(0x112,
        {op(NdOp::INT_EQUAL, reg(88, 1), {reg(0), reg(80)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(88, 1)})},
        0x130);
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x130, {op(NdOp::COPY, reg(0), {constant(23)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, supervisorReadOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2u);
  EXPECT_GT(count(R.Residual, NdOp::COND_BR), 0u);
  for (uint32_t First : {0u, 0x80000000u, UINT32_MAX})
    for (uint32_t Second : {First, First ^ 1u, First ^ 0x80000000u})
      EXPECT_EQ(executeMSRReads(R.Residual, {{x86reg::RCX, 0x10}},
                                {{0x10, First}, {0x10, Second}}),
                First == Second ? 17u : 23u);
}

TEST(InterpreterSpecialization,
     SupervisorReadRepeatedOccurrenceAndExactBudget) {
  Provider P;
  P.add(0xf0,
        {op(NdOp::COPY, reg(80), {constant(0)}),
         op(NdOp::COPY, reg(88), {constant(3)})},
        0x100);
  addMSRRead(P, 0x100);
  P.add(0x102,
        {op(NdOp::INT_ADD, reg(80), {reg(80), reg(0)}),
         op(NdOp::INT_SUB, reg(88), {reg(88), constant(1)}),
         op(NdOp::COND_BR, {}, {constant(0x100), reg(88)})},
        0x110);
  P.add(0x110, {op(NdOp::COPY, reg(0), {reg(80)}), ret()});
  auto O = supervisorReadOptions();
  O.ControlRegisters = {{88, 8}};
  const auto R = specializeInterpreter(P, {0xf0}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(
      executeMSRReads(R.Residual, {{x86reg::RCX, 0x10}},
                      {{0x10, 1}, {0x10, UINT32_MAX}, {0x10, 0x80000000u}}),
      UINT64_C(0x180000000));
  O.MaxOperations = R.EvaluatedOperations;
  EXPECT_TRUE(specializeInterpreter(P, {0xf0}, O).complete());
  --O.MaxOperations;
  const auto Short = specializeInterpreter(P, {0xf0}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     SupervisorReadRejectsIncompleteBundleAndDomain) {
  Provider P;
  addMSRRead(P, 0x100);
  P.add(0x102, {ret()});
  auto O = supervisorReadOptions();
  ASSERT_EQ(P.Code.at(0x100).Ops.size(), 6u);
  ASSERT_EQ(P.Code.at(0x100).UndefinedEffects.Coverage,
            LowUndefinedCoverage::Complete);
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Bad = P;
    auto &I = Bad.Code.at(0x100);
    switch (Mutation) {
    case 0:
      I.NativeBytes.clear();
      break;
    case 1:
      I.NativeBytes[1] = 0x30; // A write must never become a sampled read.
      break;
    case 2:
      I.NativeBytes.insert(I.NativeBytes.begin(), 0xf0);
      break;
    case 3:
      I.Ops[0].Inputs[1] = reg(x86reg::RCX, 8);
      break;
    case 4:
      I.Ops[0].Inputs[1] = reg(x86reg::RDX, 4);
      break;
    case 5:
      I.Ops[0].Output = reg(x86reg::RAX);
      break;
    case 6:
      I.Ops[1].Inputs[1] = constant(1, 4);
      break;
    case 7:
      I.Ops[2].Inputs[1] = constant(31);
      break;
    case 8:
      I.Ops[3].Inputs[0] = I.Ops[0].Output;
      break;
    case 9:
      I.Ops[4].Output = reg(x86reg::RAX, 4);
      break;
    case 10:
      I.Ops[5].Output = reg(x86reg::RCX);
      break;
    case 11:
      I.Ops[2].Output = I.Ops[0].Output;
      break;
    case 12:
      I.Ops[3].Output.Offset = UINT64_MAX - 1;
      break;
    case 13:
      I.Ops[1].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 14:
      I.IsNativeCall = true;
      break;
    case 15:
      ++I.Fallthrough.Address;
      break;
    case 16:
      ++I.Ops[3].Seq;
      break;
    case 17:
      I.Ops[0].NumInputs = 1;
      break;
    case 18:
      I.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
      break;
    case 19:
      I.UndefinedEffects.Coverage = LowUndefinedCoverage::Unsupported;
      break;
    }
    I.UndefinedEffects.OperationDigest = lowUndefinedOperationDigest(I.Ops);
    const auto R = specializeInterpreter(Bad, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  for (unsigned Missing = 0; Missing != 6; ++Missing) {
    auto Bad = O;
    if (Missing == 0)
      Bad.X64FlagsProfile.reset();
    if (Missing == 1)
      Bad.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
    if (Missing == 2)
      Bad.ExplicitMachineState = false;
    if (Missing == 3)
      Bad.NormalNonfaultingExecution = false;
    if (Missing == 4)
      Bad.ByteOrder = llvm::endianness::big;
    if (Missing == 5)
      Bad.Architecture = Arch::AArch64;
    EXPECT_FALSE(lowerRuntimeRegisterRead(P.Code.at(0x100), Bad,
                                          NdVar::tmp(0x1000, 16)));
  }
  for (auto Invalid :
       {reg(0, 16), NdVar::tmp(0x1000, 8), NdVar::tmp(UINT64_MAX - 7, 16),
        NdVar::tmp(P.Code.at(0x100).Ops[0].Output.Offset, 16)})
    EXPECT_FALSE(lowerRuntimeRegisterRead(P.Code.at(0x100), O, Invalid));
}

TEST(InterpreterSpecialization, SupervisorReadTransitionMatchesCompleteState) {
  Provider P;
  addMSRRead(P, 0x100);
  const auto &I = P.Code.at(0x100);
  const auto Sample = NdVar::tmp(0x1000, 16);
  const auto Transition =
      lowerRuntimeRegisterRead(I, supervisorReadOptions(), Sample);
  ASSERT_TRUE(Transition);
  ASSERT_EQ(Transition->SampleBytes, 8);
  ASSERT_EQ(Transition->Ops.size(), 1u);
  for (uint64_t Value :
       {UINT64_C(0), UINT64_C(0xffffffff), UINT64_C(0x8000000000000000),
        UINT64_C(0xfedcba9876543210), UINT64_MAX}) {
    SymContext C;
    SymState S(C);
    const auto Canary = [](uint64_t Offset) {
      return UINT64_C(0xdeadbeef87654321) ^ Offset;
    };
    for (uint64_t Offset = 0; Offset < 16 * 8; Offset += 8)
      S.write(SymSpace::Register, Offset, C.mkConst(64, Canary(Offset)));
    for (uint64_t Flag : x64::ModelledFlags)
      S.write(SymSpace::Register, Flag, C.mkConst(8, 1));
    const auto Vector = llvm::APInt(128, Canary(0), true);
    S.write(SymSpace::Register, x86reg::XMM0, C.mkConst(Vector));
    const auto FS = C.mkConst(64, Canary(0x100));
    const auto GS = C.mkConst(64, Canary(0x200));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FS));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, GS));
    ASSERT_TRUE(S.store(C.mkConst(64, 0x400), C.mkConst(64, Canary(0x400))));
    S.write(SymSpace::Temporary, Sample.Offset, C.mkConst(64, Value));
    SymExec E(C, S);
    for (const auto &Op : Transition->Ops)
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    for (const auto &Op : llvm::ArrayRef(I.Ops).drop_front())
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    for (uint64_t Offset = 0; Offset < 16 * 8; Offset += 8) {
      const uint64_t Expected = Offset == x86reg::RAX   ? uint32_t(Value)
                                : Offset == x86reg::RDX ? Value >> 32
                                                        : Canary(Offset);
      const auto Actual = C.asConst(S.read(SymSpace::Register, Offset, 8));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), Expected) << Offset;
    }
    for (uint64_t Flag : x64::ModelledFlags)
      EXPECT_EQ(C.asConst(S.read(SymSpace::Register, Flag, 1))->getZExtValue(),
                1u);
    EXPECT_EQ(*C.asConst(S.read(SymSpace::Register, x86reg::XMM0, 16)), Vector);
    EXPECT_EQ(C.asConst(S.load(C.mkConst(64, 0x400), 8))->getZExtValue(),
              Canary(0x400));
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS), FS);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), GS);
    EXPECT_EQ(E.opaqueOperationCount(), 0u);
  }
}

TEST(InterpreterSpecialization, SupervisorReadAddressRemainsUnknownOrigin) {
  Provider P;
  addMSRRead(P, 0x100);
  P.add(0x102, {ret()});
  auto O = ordinaryReturnOptions();
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  P.add(0x102,
        {op(NdOp::INT_LEFT, reg(x86reg::RDX), {reg(x86reg::RDX), constant(32)}),
         op(NdOp::INT_OR, reg(0), {reg(0), reg(x86reg::RDX)}),
         op(NdOp::STORE, {}, {reg(0), constant(7)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(R.Diagnostic.find("unknown address origin"), std::string::npos);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}
