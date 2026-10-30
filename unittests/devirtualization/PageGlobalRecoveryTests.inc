//===- PageGlobalRecoveryTests.inc - Bounded control-state recovery -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent value, domain, rejection and architectural-bank checks.
// Included inside the InterpreterSpecializationTests.cpp test namespace.

SpecializationOptions pageGlobalOptions() {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CR4PGEState =
      true;
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  return O;
}

LowOp readPageGlobal(NdVar Output) {
  return op(NdOp::INTRINSIC, Output,
            {constant(static_cast<uint64_t>(Intrinsic::ReadCr4), 2)});
}

LowOp writePageGlobal(NdVar Input) {
  return op(NdOp::INTRINSIC, {},
            {constant(static_cast<uint64_t>(Intrinsic::WriteCr4), 2), Input});
}

TEST(InterpreterSpecialization, PageGlobalRequiresDeclaredKernelDomain) {
  Provider P;
  P.add(0x100, {readPageGlobal(reg(0)), ret()});
  EXPECT_EQ(specializeInterpreter(P, {0x100}).Status,
            SpecializationStatus::Unsupported);
  for (unsigned Missing = 0; Missing != 7; ++Missing) {
    SCOPED_TRACE(Missing);
    auto O = pageGlobalOptions();
    if (Missing == 0)
      O.ExplicitMachineState = false;
    if (Missing == 1)
      O.NormalNonfaultingExecution = false;
    if (Missing == 2)
      O.X64FlagsProfile.reset();
    if (Missing == 3)
      O.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
    if (Missing == 4)
      O.ByteOrder = llvm::endianness::big;
    if (Missing == 5)
      O.Architecture = Arch::AArch64;
    if (Missing == 6)
      O.Architecture = Arch::X86;
    EXPECT_FALSE(x64::pageglobal::supportsOperation(readPageGlobal(reg(0)), O));
    auto Error = x64::pageglobal::validateOptions(O);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    const auto Result = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Result.Status, Missing < 5 ? SpecializationStatus::InvalidInput
                                         : SpecializationStatus::Unsupported);
    if (Missing >= 5)
      EXPECT_NE(Result.Diagnostic.find("has no adapter"), std::string::npos);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, PageGlobalReadsRetainBothUnknownBranches) {
  for (unsigned Bit : {0u, 7u, 31u, 63u}) {
    Provider P;
    P.add(0x100,
          {readPageGlobal(reg(0)),
           op(NdOp::INT_AND, reg(8), {reg(0), constant(uint64_t{1} << Bit)}),
           op(NdOp::INT_NOTEQUAL, reg(16, 1), {reg(8), constant(0)}),
           op(NdOp::COND_BR, {}, {constant(0x200), reg(16, 1)})},
          0x110);
    P.add(0x110, {ret()});
    P.add(0x200, {ret()});
    const auto Result = specializeInterpreter(P, {0x100}, pageGlobalOptions());
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 1u);
    EXPECT_EQ(count(Result.Residual, NdOp::RETURN), 2u);
  }
}

TEST(InterpreterSpecialization, PageGlobalWritesKeepFullWidthAndReadRelations) {
  for (auto Code : {NdOp::INT_OR, NdOp::INT_AND, NdOp::INT_XOR}) {
    Provider P;
    P.add(0x100,
          {readPageGlobal(reg(0)),
           op(Code, reg(8),
              {reg(0), constant(Code == NdOp::INT_AND ? ~uint64_t{0x80}
                                                      : uint64_t{0x80})}),
           writePageGlobal(reg(8)), readPageGlobal(reg(24)),
           op(NdOp::INT_NOTEQUAL, reg(16, 1), {reg(8), reg(24)}),
           op(NdOp::COND_BR, {}, {constant(0xbad), reg(16, 1)})},
          0x110);
    P.add(0x110, {ret()});
    const auto Result = specializeInterpreter(P, {0x100}, pageGlobalOptions());
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 3u);
    EXPECT_EQ(count(Result.Residual, NdOp::RETURN), 1u);
  }
}

TEST(InterpreterSpecialization, PageGlobalRejectsEveryOtherControlBit) {
  for (unsigned Bit = 0; Bit != 64; ++Bit) {
    Provider P;
    P.add(0x100,
          {readPageGlobal(reg(0)),
           op(NdOp::INT_XOR, reg(0), {reg(0), constant(uint64_t{1} << Bit)}),
           writePageGlobal(reg(0)), ret()});
    const auto Result = specializeInterpreter(P, {0x100}, pageGlobalOptions());
    if (Bit == 7) {
      EXPECT_TRUE(Result.complete()) << Result.Diagnostic;
    } else {
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
      EXPECT_NE(Result.Diagnostic.find("non-PGE control bit"),
                std::string::npos);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
    }
  }
  Provider P;
  P.add(0x100, {writePageGlobal(constant(0)), ret()});
  EXPECT_EQ(specializeInterpreter(P, {0x100}, pageGlobalOptions()).Status,
            SpecializationStatus::Unsupported);
}

TEST(InterpreterSpecialization, PageGlobalTransitionPreservesOtherBanks) {
  for (bool MXCSR : {false, true})
    for (bool GS : {false, true})
      for (uint64_t Changed :
           {uint64_t{0}, uint64_t{0x80}, uint64_t{0x20}, uint64_t{1} << 63}) {
        SymContext Ctx;
        SymState State(Ctx);
        auto O = pageGlobalOptions();
        O.X64LegacyGSState = GS;
        if (MXCSR)
          O.X64MXCSRMask = 0xffbf;
        const auto Base = Ctx.mkConst(64, 0x123456780000);
        ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(
            NdMemoryAddressSpace::X86GS, Base));
        auto Controls = seedRuntimeControlState(Ctx, State, O);
        ASSERT_EQ(Controls.Values.size(), size_t(MXCSR) + GS + 1);
        Controls.Values.back() = Ctx.mkConst(64, 0x1234567800030620);
        const auto Before = Controls.Values;
        uint64_t Next = 0x10000;
        const auto Value = uint64_t{0x1234567800030620} ^ Changed;
        auto Transition = lowerRuntimeControlOperation(
            writePageGlobal(constant(Value)), State, Controls, O,
            [&](uint16_t Size) {
              const auto V = NdVar::tmp(Next, Size);
              Next += 8;
              return V;
            });
        ASSERT_TRUE(Transition);
        ASSERT_TRUE(Transition->Rejected);
        ASSERT_TRUE(Transition->OutputState);
        EXPECT_EQ(Transition->OutputState->first, Before.size() - 1);
        SymExec Exec(Ctx, State);
        ASSERT_EQ(Exec.run(Transition->Ops), Transition->Ops.size());
        const bool Bad = Changed != 0 && Changed != 0x80;
        const auto Rejected =
            Ctx.asConst(Exec.operandValue(*Transition->Rejected));
        ASSERT_TRUE(Rejected);
        EXPECT_EQ(Rejected->getZExtValue(), uint64_t(Bad));
        const auto Output = Exec.operandValue(Transition->OutputState->second);
        const auto Concrete = Ctx.asConst(Output);
        ASSERT_TRUE(Concrete);
        EXPECT_EQ(Concrete->getZExtValue(), Bad ? 0x1234567800030620 : Value);
        if (!Bad) {
          auto Error = commitRuntimeControlState(
              State, Controls, Transition->OutputState->first, Output, O);
          ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
        }
        for (size_t Index = 0; Index + 1 < Before.size(); ++Index)
          EXPECT_EQ(Controls.Values[Index], Before[Index]);
        EXPECT_EQ(State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
                  Base);
      }
}

TEST(InterpreterSpecialization,
     PageGlobalRejectsMalformedOperationsAndBudgets) {
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    auto Op = Kind < 5 ? readPageGlobal(reg(0)) : writePageGlobal(reg(0));
    if (Kind == 0)
      Op.Output.Size = 4;
    if (Kind == 1)
      Op.Output = constant(0);
    if (Kind == 2)
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Kind == 3)
      Op.Inputs[0].Size = 8;
    if (Kind == 4)
      Op.addInput(constant(0));
    if (Kind == 5)
      Op.Inputs[1].Size = 4;
    if (Kind == 6)
      Op.Output = reg(8);
    if (Kind == 7)
      Op.Inputs[1] = NdVar::tmp(UINT64_MAX - 3, 8);
    EXPECT_FALSE(x64::pageglobal::supportsOperation(Op, pageGlobalOptions()));
  }
  Provider P;
  P.add(0x100, {readPageGlobal(reg(0)), writePageGlobal(reg(0)), ret()});
  auto O = pageGlobalOptions();
  O.MaxOperations = 5;
  const auto Result = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}
