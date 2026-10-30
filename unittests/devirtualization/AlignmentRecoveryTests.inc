//===- AlignmentRecoveryTests.inc - Checked alignment recovery ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent alignment predicates, refusal domains and proof budgets.
//===----------------------------------------------------------------------===//

SpecializationOptions alignmentOptions() {
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  return Options;
}

LowOp requireAlignment(NdVar Address, uint64_t Alignment = 16) {
  return op(NdOp::INTRINSIC, {},
            {constant(uint64_t(Intrinsic::RequireAligned), 2), Address,
             constant(Alignment)});
}

TEST(InterpreterSpecialization, AlignmentPredicateMatchesIndependentRemainder) {
  for (unsigned Shift : {0U, 1U, 4U, 6U, 31U, 32U, 63U}) {
    SCOPED_TRACE(Shift);
    const uint64_t Alignment = uint64_t{1} << Shift;
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    const auto Address = C.mkVar("address", 64);
    S.write(SymSpace::Register, 8, Address);
    S.write(SymSpace::Register, 16, C.mkConst(64, 0xabc));
    S.store(C.mkConst(64, 0x2000), C.mkConst(64, 0xdef));
    auto Options = alignmentOptions();
    auto Controls = seedRuntimeControlState(C, Options);
    ASSERT_TRUE(Controls.Values.empty());
    auto Original = requireAlignment(reg(8), Alignment);
    Original.Addr = 0x100;
    Original.Seq = 7;
    const auto Digest = lowUndefinedOperationDigest({Original});
    uint64_t Next = 0x1000;
    unsigned Allocations = 0;
    const auto Transition = lowerRuntimeControlOperation(
        Original, S, Controls, Options, [&](uint16_t Bytes) {
          ++Allocations;
          const auto Value = NdVar::tmp(Next, Bytes);
          Next += Bytes;
          return Value;
        });
    ASSERT_TRUE(Transition);
    ASSERT_TRUE(Transition->Rejected);
    EXPECT_FALSE(Transition->OutputState);
    ASSERT_EQ(Transition->Ops.size(), 2U);
    EXPECT_EQ(Allocations, 2U);
    EXPECT_EQ(Next, 0x1009U);
    for (unsigned I = 0; I != Transition->Ops.size(); ++I) {
      const auto &Scalar = Transition->Ops[I];
      EXPECT_EQ(Scalar.Addr, Original.Addr);
      EXPECT_EQ(Scalar.Seq, int(I));
      EXPECT_TRUE(Scalar.Output.isTemp());
      ASSERT_EQ(E.step(Scalar), StepResult::Continue);
    }
    const auto Rejected = E.operandValue(*Transition->Rejected);
    // Independent arithmetic specification: the remainder is zero exactly
    // when the address is a multiple of the declared positive alignment.
    const auto Remainder = C.mkURem(Address, C.mkConst(64, Alignment));
    const auto Oracle = C.mkZExt(C.mkNe(Remainder, C.mkZero(64)), 8);
    solver::BitVectorSolver Proof(C);
    ASSERT_TRUE(Proof.assertDistinct(Rejected, Oracle));
    EXPECT_EQ(Proof.check(), solver::SatResult::Unsat);
    EXPECT_EQ(S.read(SymSpace::Register, 8, 8), Address);
    EXPECT_EQ(C.asConst(S.read(SymSpace::Register, 16, 8))->getZExtValue(),
              0xabcU);
    EXPECT_EQ(C.asConst(S.load(C.mkConst(64, 0x2000), 8))->getZExtValue(),
              0xdefU);
    EXPECT_EQ(lowUndefinedOperationDigest({Original}), Digest);
    EXPECT_EQ(E.opaqueOperationCount(), 0U);
  }
}

TEST(InterpreterSpecialization, AlignmentRejectsUnsupportedShapesAndDomains) {
  const auto Good = requireAlignment(reg(8));
  std::vector<LowOp> Invalid;
  for (unsigned Case = 0; Case != 15; ++Case) {
    LowOp Bad = Good;
    switch (Case) {
    case 0:
      Bad.Inputs[0].Size = 1;
      break;
    case 1:
      Bad.Inputs[0] = reg(0, 2);
      break;
    case 2:
      Bad.Output = reg(24, 1);
      break;
    case 3:
      Bad.Inputs[1].Size = 4;
      break;
    case 4:
      Bad.Inputs[1].Size = 16;
      break;
    case 5:
      Bad.Inputs[1] = NdVar::tmp(UINT64_MAX - 3, 8);
      break;
    case 6:
      Bad.Inputs[2] = constant(0);
      break;
    case 7:
      Bad.Inputs[2] = constant(3);
      break;
    case 8:
      Bad.Inputs[2] = reg(16);
      break;
    case 9:
      Bad.Inputs[2].Size = 4;
      break;
    case 10:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 11:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 12:
      Bad.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 13:
      Bad.NumInputs = 2;
      break;
    case 14:
      Bad.MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(255);
      break;
    }
    Invalid.push_back(Bad);
  }
  for (auto Guard :
       {constant(0, 1), constant(1, 1), constant(255, 1), reg(24, 1)}) {
    auto Bad = Good;
    Bad.addInput(Guard);
    Invalid.push_back(Bad);
  }
  SymContext C;
  SymState S(C);
  RuntimeControlState Controls;
  for (const auto &Bad : Invalid) {
    unsigned Allocations = 0;
    EXPECT_FALSE(supportsRuntimeControlOperation(Bad, alignmentOptions()));
    EXPECT_FALSE(lowerRuntimeControlOperation(
        Bad, S, Controls, alignmentOptions(), [&](uint16_t Bytes) {
          ++Allocations;
          return NdVar::tmp(0x1000, Bytes);
        }));
    EXPECT_EQ(Allocations, 0U);
    Provider P;
    P.add(0x100, {Bad, ret()});
    const auto Result = specializeInterpreter(P, {0x100}, alignmentOptions());
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
  for (unsigned Case = 0; Case != 6; ++Case) {
    auto Options = alignmentOptions();
    switch (Case) {
    case 0:
      Options.ExplicitMachineState = false;
      break;
    case 1:
      Options.NormalNonfaultingExecution = false;
      break;
    case 2:
      Options.ByteOrder = llvm::endianness::big;
      break;
    case 3:
      Options.Architecture = Arch::X86;
      break;
    case 4:
      Options.Architecture = Arch::AArch64;
      break;
    case 5:
      Options.Architecture = Arch::Unknown;
      break;
    }
    EXPECT_FALSE(supportsRuntimeControlOperation(Good, Options));
    EXPECT_FALSE(lowerRuntimeControlOperation(
        Good, S, Controls, Options,
        [](uint16_t Bytes) { return NdVar::tmp(0x1000, Bytes); }));
  }
}

TEST(InterpreterSpecialization, AlignmentScratchRefusesAliasesAndWrapping) {
  const auto Address = NdVar::tmp(0x100, 8);
  const std::pair<NdVar, NdVar> Invalid[] = {
      {reg(0, 8), NdVar::tmp(0x1008, 1)},
      {NdVar::tmp(0x1000, 4), NdVar::tmp(0x1008, 1)},
      {NdVar::tmp(UINT64_MAX - 3, 8), NdVar::tmp(0x1008, 1)},
      {NdVar::tmp(0x1000, 8), reg(0, 1)},
      {NdVar::tmp(0x1000, 8), NdVar::tmp(0x1008, 8)},
      {NdVar::tmp(0x1000, 8), NdVar::tmp(0x1000, 1)},
      {NdVar::tmp(0x1000, 8), NdVar::tmp(0x1007, 1)},
      {Address, NdVar::tmp(0x1008, 1)},
      {NdVar::tmp(0xfc, 8), NdVar::tmp(0x1008, 1)},
      {NdVar::tmp(0x1000, 8), NdVar::tmp(0x107, 1)}};
  for (const auto &[First, Second] : Invalid) {
    SymContext C;
    SymState S(C);
    const auto Value = C.mkVar("address", 64);
    S.write(SymSpace::Temporary, Address.Offset, Value);
    const auto Nodes = C.numNodes();
    unsigned Allocations = 0;
    EXPECT_FALSE(lowerRuntimeControlOperation(
        requireAlignment(Address), S, {}, alignmentOptions(),
        [&](uint16_t) { return Allocations++ ? Second : First; }));
    EXPECT_EQ(Allocations, 2U);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(S.read(SymSpace::Temporary, Address.Offset, 8), Value);
  }
}

TEST(InterpreterSpecialization, AlignmentRetainsIntrinsicWithoutMXCSR) {
  for (uint64_t Alignment : {uint64_t{1}, uint64_t{2}, uint64_t{16},
                             uint64_t{64}, uint64_t{1} << 63}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::INT_AND, reg(16), {reg(8), constant(~(Alignment - 1))}),
           requireAlignment(reg(16), Alignment),
           op(NdOp::COPY, reg(0), {reg(16)}), ret()});
    auto Options = alignmentOptions();
    ASSERT_FALSE(Options.X64MXCSRMask);
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 1U);
    for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{15},
                           UINT64_C(0x123456789abcdef0), UINT64_MAX})
      EXPECT_EQ(execute(Result.Residual, {{8, Input}}),
                Input - Input % Alignment);
  }
  // A separate state owner still sees the control value across an alignment
  // transition; the latter must neither require nor consume the MXCSR slot.
  Provider P;
  P.add(0x100, {op(NdOp::INTRINSIC, {},
                   {constant(uint64_t(Intrinsic::X86WriteMXCSR), 2),
                    constant(0x1f80, 4)}),
                requireAlignment(constant(0x1000)),
                op(NdOp::INTRINSIC, reg(8, 4),
                   {constant(uint64_t(Intrinsic::X86ReadMXCSR), 2)}),
                op(NdOp::INT_ZEXT, reg(0), {reg(8, 4)}), ret()});
  auto Options = alignmentOptions();
  Options.X64MXCSRMask = 0xffbf;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 3U);
  for (uint32_t Initial : {0U, 0x8000U, 0xffbfU}) {
    uint32_t Final = 0;
    EXPECT_EQ(execute(Result.Residual, {}, {}, 0, {}, {}, {}, nullptr, Initial,
                      0xffbf, &Final),
              0x1f80U);
    EXPECT_EQ(Final, 0x1f80U);
  }
}

TEST(InterpreterSpecialization, AlignmentNonfaultingFlagCannotProveValidity) {
  for (auto Address :
       {reg(8), constant(1), constant(0x1001), constant(UINT64_MAX)}) {
    Provider P;
    P.add(0x100, {requireAlignment(Address), ret()});
    const auto Result = specializeInterpreter(P, {0x100}, alignmentOptions());
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(Result.Diagnostic.find("alignment"), std::string::npos);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, AlignmentBranchProofAndExactBudgets) {
  Provider P;
  // The full input is arbitrary; the small projected address domain keeps the
  // test independent of unbounded-address context refinement heuristics.
  P.add(0x100,
        {op(NdOp::INT_AND, reg(8), {reg(8), constant(31)}),
         op(NdOp::INT_AND, reg(16), {reg(8), constant(15)}),
         op(NdOp::INT_EQUAL, reg(24, 1), {reg(16), constant(0)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(24, 1)})},
        0x120);
  P.add(0x110,
        {requireAlignment(reg(8)), op(NdOp::COPY, reg(0), {reg(8)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(999)}), ret()});
  auto Options = alignmentOptions();
  Options.ControlRegisters = {{8, 8}};
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 1U);
  ASSERT_GT(Result.EvaluatedOperations, 1U);
  ASSERT_GT(Result.SolverQueries, 1U);
  for (uint64_t High : {uint64_t{0}, UINT64_C(0xcafe123400000000)})
    for (unsigned Low = 0; Low != 32; ++Low)
      EXPECT_EQ(execute(Result.Residual, {{8, High | Low}}),
                Low % 16 == 0 ? uint64_t(Low) : 999U);

  Options.MaxOperations = Result.EvaluatedOperations;
  Options.MaxSolverQueries = Result.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, Options).complete());
  for (bool Queries : {false, true}) {
    auto Short = Options;
    if (Queries)
      --Short.MaxSolverQueries;
    else
      --Short.MaxOperations;
    const auto Failure = specializeInterpreter(P, {0x100}, Short);
    EXPECT_EQ(Failure.Status, SpecializationStatus::BudgetExceeded)
        << Failure.Diagnostic;
    EXPECT_TRUE(Failure.Residual.Blocks.empty());
  }
  auto SmallNodes = Options;
  SmallNodes.MaxSymbolicNodes = 1;
  const auto Failure = specializeInterpreter(P, {0x100}, SmallNodes);
  EXPECT_EQ(Failure.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Failure.Residual.Blocks.empty());

  // A predicate on a different input grants no alignment fact for the address.
  P.Code.at(0x100).Ops[1].Inputs[0] = reg(40);
  const auto Unrelated = specializeInterpreter(P, {0x100}, alignmentOptions());
  EXPECT_EQ(Unrelated.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Unrelated.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, AlignmentSurvivesOrdinaryNodeCuts) {
  for (uint64_t Alignment : {16u, 64u, 256u}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::INT_AND, reg(16), {reg(8), constant(~(Alignment - 1))}),
           jump(0x110)});
    P.add(0x110, {requireAlignment(reg(16), Alignment),
                  op(NdOp::COPY, reg(0), {reg(16)}), ret()});
    auto Options = alignmentOptions();
    Options.DiscoverControlState = true;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 1u);
    if (Alignment < 256) {
      EXPECT_GT(Result.ControlRefinements, 0u);
      auto WithoutDiscovery = Options;
      WithoutDiscovery.DiscoverControlState = false;
      EXPECT_FALSE(
          specializeInterpreter(P, {0x100}, WithoutDiscovery).complete());
    }
    for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{63},
                           UINT64_C(0x123456789abcdef0), UINT64_MAX})
      EXPECT_EQ(execute(Result.Residual, {{8, Input}}),
                Input - Input % Alignment);
    Options.MaxSolverQueries = Result.SolverQueries;
    Options.MaxOperations = Result.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, Options).complete());
    for (bool Queries : {false, true}) {
      auto Short = Options;
      if (Queries)
        --Short.MaxSolverQueries;
      else
        --Short.MaxOperations;
      const auto Failure = specializeInterpreter(P, {0x100}, Short);
      EXPECT_EQ(Failure.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Failure.Residual.Blocks.empty());
    }
  }
}

TEST(InterpreterSpecialization, AlignmentSurvivesLoopWidening) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_AND, reg(16), {reg(8), constant(~uint64_t{15})}),
         op(NdOp::INT_AND, reg(24), {reg(8), constant(3)}),
         op(NdOp::INT_ADD, reg(24), {reg(24), constant(1)}), jump(0x110)});
  P.add(0x110,
        {requireAlignment(reg(16)),
         op(NdOp::INT_ADD, reg(16), {reg(16), constant(16)}),
         op(NdOp::INT_SUB, reg(24), {reg(24), constant(1)}),
         op(NdOp::INT_NOTEQUAL, reg(32, 1), {reg(24), constant(0)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(32, 1)})},
        0x120);
  P.add(0x120, {op(NdOp::COPY, reg(0), {reg(16)}), ret()});
  auto Options = alignmentOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{3},
                         UINT64_C(0x123456789abcdef0), UINT64_MAX})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}),
              (Input & ~uint64_t{15}) + 16 * ((Input & 3) + 1));
}

TEST(InterpreterSpecialization, AlignmentDiscoveryKeepsInvalidIncomingArms) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    Provider P;
    P.add(0x100,
          {op(NdOp::INT_EQUAL, reg(32, 1), {reg(24), constant(0)}),
           op(NdOp::COND_BR, {}, {constant(0x110), reg(32, 1)})},
          0x120);
    P.add(0x110, {op(NdOp::INT_AND, reg(16), {reg(8), constant(~uint64_t{15})}),
                  jump(0x130)});
    P.add(0x120,
          {op(NdOp::INT_AND, reg(16), {reg(8), constant(~uint64_t{15})}),
           Kind == 2 ? op(NdOp::COPY, reg(16), {reg(40)})
                     : op(NdOp::INT_ADD, reg(16), {reg(16), constant(Kind)}),
           jump(0x130)});
    P.add(0x130, {requireAlignment(reg(16)), op(NdOp::COPY, reg(0), {reg(16)}),
                  ret()});
    auto Options = alignmentOptions();
    Options.DiscoverControlState = true;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.complete(), Kind == 0)
        << Kind << ": " << Result.Diagnostic;
    if (Kind)
      EXPECT_TRUE(Result.Residual.Blocks.empty());
    else if (Result.complete())
      for (uint64_t Branch : {0u, 1u})
        EXPECT_EQ(execute(Result.Residual, {{8, 53}, {24, Branch}}), 48u);
  }
}

TEST(InterpreterSpecialization, RuntimeValidityDiscoveryTracksMaskedMXCSR) {
  for (bool Mask : {false, true}) {
    Provider P;
    P.add(0x100, {op(NdOp::INT_AND, reg(16, 4),
                     {reg(8, 4), constant(Mask ? 0xffbf : UINT32_MAX, 4)}),
                  jump(0x110)});
    P.add(0x110,
          {op(NdOp::INTRINSIC, {},
              {constant(uint64_t(Intrinsic::X86WriteMXCSR), 2), reg(16, 4)}),
           op(NdOp::INTRINSIC, reg(0, 4),
              {constant(uint64_t(Intrinsic::X86ReadMXCSR), 2)}),
           op(NdOp::INT_ZEXT, reg(0), {reg(0, 4)}), ret()});
    auto Options = alignmentOptions();
    Options.X64MXCSRMask = 0xffbf;
    Options.DiscoverControlState = true;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.complete(), Mask) << Result.Diagnostic;
    if (!Mask) {
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      continue;
    }
    ASSERT_TRUE(Result.complete());
    for (uint64_t Input : {uint64_t{0}, uint64_t{64}, uint64_t{0x1f80},
                           uint64_t{0x8040}, UINT64_MAX}) {
      uint32_t Final = 0;
      EXPECT_EQ(execute(Result.Residual, {{8, Input}}, {}, 0, {}, {}, {},
                        nullptr, 0, 0xffbf, &Final),
                Input & 0xffbf);
      EXPECT_EQ(Final, Input & 0xffbf);
    }
  }
}
