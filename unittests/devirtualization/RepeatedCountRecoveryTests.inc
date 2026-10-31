//===- RepeatedCountRecoveryTests.inc - Finite repeat dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Live count partitions, exhaustive predecessors and work bounds.

Provider repeatedCountProvider(bool Branch, bool Unbounded = false,
                               bool PrefixWrite = false) {
  Provider P;
  if (Branch) {
    P.add(0x100,
          {op(NdOp::INT_NOTEQUAL, reg(48, 1), {reg(40), constant(0)}),
           op(NdOp::COND_BR, {}, {constant(0x120), reg(48, 1)})},
          0x110);
    P.add(0x110,
          {op(NdOp::INT_AND, reg(8), {reg(8), constant(1)}), jump(0x130)});
    P.add(0x120,
          {op(Unbounded ? NdOp::COPY : NdOp::INT_AND, reg(8),
              Unbounded ? std::initializer_list<NdVar>{reg(8)}
                        : std::initializer_list<NdVar>{reg(8), constant(3)}),
           jump(0x130)});
  } else {
    P.add(0x100,
          {op(PrefixWrite ? NdOp::COPY : NdOp::INT_AND, reg(8),
              PrefixWrite ? std::initializer_list<NdVar>{reg(8)}
                          : std::initializer_list<NdVar>{reg(8), constant(3)})},
          0x130);
  }
  std::vector<LowOp> Ops;
  if (PrefixWrite)
    Ops.push_back(op(NdOp::INT_AND, reg(8), {reg(8), constant(3)}));
  Ops.push_back(op(NdOp::INTRINSIC, NdVar::tmp(1024, 8),
                   {constant(uint64_t(Intrinsic::Stosb), 2), reg(16), reg(8),
                    reg(24, 1), reg(32, 1)}));
  Ops.push_back(op(NdOp::COPY, reg(0), {reg(8)}));
  P.add(0x130, std::move(Ops), 0x140);
  P.add(0x140, {ret()});
  return P;
}

TEST(InterpreterSpecialization, RepeatedCountsUseExhaustiveLivePartitions) {
  for (bool Branch : {false, true}) {
    auto P = repeatedCountProvider(Branch);
    SpecializationOptions Options;
    Options.DiscoverControlState = true;
    Options.MaxChainedTransfers = 32;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.DiscoveredContextFields, 0u);
    for (uint64_t Input : {UINT64_C(0), UINT64_C(1), UINT64_C(2), UINT64_C(3),
                           UINT64_C(4), UINT64_MAX})
      for (uint64_t Selector : {UINT64_C(0), UINT64_MAX})
        for (uint64_t Direction : {UINT64_C(0), UINT64_C(128)}) {
          std::map<uint64_t, uint8_t> Memory, Actual;
          for (uint64_t I = 0; I != 16; ++I)
            Memory[0x2000 + I] = uint8_t(13 * I);
          auto Expected = Memory;
          const auto Count = Input & (Branch && !Selector ? 1 : 3);
          for (uint64_t I = 0; I != Count; ++I)
            Expected[Direction ? 0x2008 - I : 0x2008 + I] = 0xa7;
          EXPECT_EQ(execute(Result.Residual,
                            {{8, Input},
                             {16, 0x2008},
                             {24, 0xa7},
                             {32, Direction},
                             {40, Selector}},
                            Memory, 0, {}, {}, {}, &Actual),
                    Count);
          EXPECT_EQ(Actual, Expected);
        }
    for (bool Queries : {false, true}) {
      auto Exact = Options;
      auto &Limit = Queries ? Exact.MaxSolverQueries : Exact.MaxOperations;
      Limit = Queries ? Result.SolverQueries : Result.EvaluatedOperations;
      ASSERT_GT(Limit, 0u);
      ASSERT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
      --Limit;
      const auto Short = specializeInterpreter(P, {0x100}, Exact);
      EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Short.Residual.Blocks.empty());
    }
  }
}

TEST(InterpreterSpecialization, RepeatedCountsRefuseIncompletePartitions) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    // A later unbounded predecessor must not inherit an earlier finite set.
    // A write within the repeated instruction cannot be tested at its entry.
    auto P = repeatedCountProvider(Case == 0, Case == 0, Case == 1);
    SpecializationOptions Options;
    Options.DiscoverControlState = Case != 2;
    Options.MaxChainedTransfers = 32;
    if (Case == 3)
      Options.MaxControlTuples = 1;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_FALSE(Result.complete()) << Result.Diagnostic;
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}
