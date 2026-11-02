//===- ControlRangeTests.inc - Unsigned ranges across recovery cuts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bounded-length and modular-underflow recovery programs.
//===----------------------------------------------------------------------===//

Provider boundedLengthProvider(bool Frame, bool Invalid) {
  Provider P;
  std::vector<LowOp> Entry = {
      op(NdOp::INT_ZEXT, reg(16), {reg(8, 2)}),
      op(NdOp::INT_LESS, reg(48, 1), {reg(16), constant(518)}),
      op(NdOp::SELECT, reg(16), {reg(48, 1), reg(16), constant(518)})};
  if (Frame)
    Entry.push_back(op(NdOp::STORE, {}, {reg(64), reg(16)}));
  Entry.push_back(
      op(NdOp::INT_LESS, reg(48, 1), {reg(16), constant(Invalid ? 0 : 79)}));
  Entry.push_back(op(NdOp::COND_BR, {}, {constant(0x150), reg(48, 1)}));
  P.add(0x100, Entry, 0x110);
  std::vector<LowOp> Body;
  if (Frame)
    Body.push_back(op(NdOp::LOAD, reg(16), {reg(64)}));
  Body.push_back(op(NdOp::INT_AND, reg(24), {reg(24), constant(15)}));
  Body.push_back(op(NdOp::INT_SUB, reg(16), {reg(16), reg(24)}));
  Body.push_back(op(NdOp::INT_RIGHT, reg(32), {reg(16), constant(5)}));
  Body.push_back(op(NdOp::INT_LESS, reg(48, 1), {constant(8192), reg(32)}));
  Body.push_back(op(NdOp::COND_BR, {}, {constant(0x140), reg(48, 1)}));
  P.add(0x110, Body, 0x150);
  // No semantics are supplied for this arm. Only an exhaustive reachability
  // proof may exclude it; deleting it would hide a real modular underflow.
  P.add(0x140, {op(NdOp::INTRINSIC, {}, {constant(0xffff, 2)}), ret()});
  P.add(0x150, {op(NdOp::COPY, reg(0), {reg(16)}), ret()});
  return P;
}

TEST(InterpreterSpecialization, UnsignedRangesPreserveBoundedSubtraction) {
  for (bool Frame : {false, true})
    for (uint32_t Chained : {0u, 512u}) {
      SCOPED_TRACE(Frame);
      SCOPED_TRACE(Chained);
      auto P = boundedLengthProvider(Frame, false);
      auto Options = alignmentOptions();
      Options.DiscoverControlState = true;
      Options.MaxChainedTransfers = Chained;
      if (Frame)
        Options.FrameBaseRegister = SymRegisterRange{64, 8};
      const auto Result = specializeInterpreter(P, {0x100}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 0u);
      for (unsigned Input : {0u, 1u, 78u, 79u, 80u, 517u, 518u, 519u, 65535u})
        for (unsigned Adjustment = 0; Adjustment < 16; ++Adjustment) {
          const uint64_t Bounded = std::min(Input, 518u);
          const uint64_t Expected =
              Bounded < 79 ? Bounded : Bounded - Adjustment;
          EXPECT_EQ(execute(Result.Residual,
                            {{8, UINT64_C(0xfedcba9876540000) | Input},
                             {24, UINT64_C(0xabcdef0000000000) | Adjustment},
                             {64, 0x800}}),
                    Expected);
        }
      if (!Frame && !Chained) {
        Options.MaxOperations = Result.EvaluatedOperations;
        Options.MaxSolverQueries = Result.SolverQueries;
        ASSERT_TRUE(specializeInterpreter(P, {0x100}, Options).complete());
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
}

TEST(InterpreterSpecialization, UnsignedRangesKeepReachableUnderflow) {
  for (bool Frame : {false, true}) {
    auto P = boundedLengthProvider(Frame, true);
    auto Options = alignmentOptions();
    Options.DiscoverControlState = true;
    if (Frame)
      Options.FrameBaseRegister = SymRegisterRange{64, 8};
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, UnsignedRangesKeepMaskedCarrierPayload) {
  for (bool Invalid : {false, true})
    for (uint32_t Chained : {0u, 512u}) {
      SCOPED_TRACE(Invalid);
      SCOPED_TRACE(Chained);
      Provider P;
      P.add(0x100,
            {op(NdOp::INT_AND, reg(16), {reg(8), constant(65535)}),
             op(NdOp::INT_LESS, reg(48, 1), {reg(16), constant(50000)}),
             op(NdOp::SELECT, reg(16),
                {reg(48, 1), reg(16), constant(Invalid ? 60000 : 50000)}),
             op(NdOp::INT_AND, reg(24), {reg(24), constant(~UINT64_C(65535))}),
             op(NdOp::INT_OR, reg(16), {reg(16), reg(24)}),
             op(NdOp::BRANCH, {}, {constant(0x110)})});
      P.add(0x110,
            {op(NdOp::INT_AND, reg(32), {reg(16), constant(65535)}),
             op(NdOp::INT_RIGHT, reg(32), {reg(32), constant(5)}),
             op(NdOp::INT_LESS, reg(48, 1), {constant(1562), reg(32)}),
             op(NdOp::COND_BR, {}, {constant(0x140), reg(48, 1)})},
            0x150);
      P.add(0x140, {op(NdOp::INTRINSIC, {}, {constant(0xffff, 2)}), ret()});
      P.add(0x150, {op(NdOp::COPY, reg(0), {reg(16)}), ret()});
      auto Options = alignmentOptions();
      Options.DiscoverControlState = true;
      Options.MaxChainedTransfers = Chained;
      const auto Result = specializeInterpreter(P, {0x100}, Options);
      if (Invalid) {
        EXPECT_FALSE(Result.complete());
        EXPECT_TRUE(Result.Residual.Blocks.empty());
        continue;
      }
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 0u);
      for (uint64_t Payload :
           {UINT64_C(0), UINT64_C(0xfedcba9876540000), UINT64_MAX})
        for (unsigned Input : {0u, 31u, 32u, 49999u, 50000u, 50001u, 65535u})
          EXPECT_EQ(execute(Result.Residual, {{8, Input}, {24, Payload}}),
                    (Payload & ~UINT64_C(65535)) | std::min(Input, 50000u));
    }
}

TEST(InterpreterSpecialization, UnsignedRangesDropMissingPredecessorBound) {
  Provider P;
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x130), reg(40, 1)})}, 0x120);
  P.add(0x120,
        {op(NdOp::INT_AND, reg(16), {reg(8), constant(65535)}),
         op(NdOp::INT_LESS, reg(48, 1), {reg(16), constant(50000)}),
         op(NdOp::SELECT, reg(16), {reg(48, 1), reg(16), constant(50000)}),
         op(NdOp::BRANCH, {}, {constant(0x150)})});
  P.add(0x130, {op(NdOp::COPY, reg(16), {reg(8)}),
                op(NdOp::BRANCH, {}, {constant(0x150)})});
  P.add(0x150,
        {op(NdOp::INT_LESS, reg(48, 1), {constant(50000), reg(16)}),
         op(NdOp::COND_BR, {}, {constant(0x170), reg(48, 1)})},
        0x160);
  P.add(0x160, {op(NdOp::COPY, reg(0), {reg(16)}), ret()});
  P.add(0x170, {op(NdOp::INTRINSIC, {}, {constant(0xffff, 2)}), ret()});
  auto Options = alignmentOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, UnsignedRangesPreserveGuardedLoopBound) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_AND, reg(16), {reg(8), constant(65535)}),
         op(NdOp::INT_LESS, reg(48, 1), {reg(16), constant(50000)}),
         op(NdOp::SELECT, reg(16), {reg(48, 1), reg(16), constant(50000)}),
         op(NdOp::BRANCH, {}, {constant(0x110)})});
  P.add(0x110,
        {op(NdOp::INT_EQUAL, reg(48, 1), {reg(16), constant(0)}),
         op(NdOp::COND_BR, {}, {constant(0x150), reg(48, 1)})},
        0x120);
  P.add(0x120,
        {op(NdOp::INT_SUB, reg(16), {reg(16), constant(1)}),
         op(NdOp::INT_LESS, reg(48, 1), {constant(50000), reg(16)}),
         op(NdOp::COND_BR, {}, {constant(0x140), reg(48, 1)})},
        0x110);
  P.add(0x140, {op(NdOp::INTRINSIC, {}, {constant(0xffff, 2)}), ret()});
  P.add(0x150, {op(NdOp::COPY, reg(0), {reg(16)}), ret()});
  auto Options = alignmentOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 0u);
  for (unsigned Input : {0u, 1u, 15u, 32u, 127u})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}), 0u);
}
