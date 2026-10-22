//===- ReturningCallIntegrityTests.inc - Exact call-span controls --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Direct semantic validator controls, independent of the outer graph hash.
//===----------------------------------------------------------------------===//

TEST_F(PEFixedImageTest, ReturningCallSemanticValidatorRejectsResealedSpans) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0, 0xff, 0xd0, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto O = returningCallOptions(Base + 0x1000, Base + 0x1004);
  O.X64ReturningCalls.push_back(
      {1, Base + 0x1002,
       SpecializationX64CallProfile::ConditionalNormalReturnV1, 32});
  const auto Good = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  auto Valid = x64::validateReturningCalls(Good, O);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  for (unsigned Change = 0; Change < 10; ++Change) {
    SCOPED_TRACE(Change);
    auto R = Good;
    auto &C = R.ReturningCalls.front();
    bool Found = false;
    for (auto &B : R.Residual.Blocks)
      for (auto &I : B.InstructionBoundaries)
        if (I.Address == C.ResidualAddress) {
          Found = true;
          auto Span =
              llvm::MutableArrayRef<LowOp>(B.Ops).slice(I.FirstOp, I.OpCount);
          switch (Change) {
          case 0:
            // Valid-shaped target replacement, not a stale outer checksum.
            Span.back().Inputs[5] = NdVar::reg(x86reg::R11, 8);
            break;
          case 1:
            for (auto &Op : Span)
              if (Op.Opcode == NdOp::STORE) {
                auto M = lowMemoryOperands(Op);
                Op.Inputs[M.StoredValue - Op.Inputs] =
                    NdVar::scalar(C.Original.Fallthrough.Address + 1, 8);
              }
            break;
          case 2:
            ASSERT_GE(Span.size(), 3U);
            std::swap(Span[0], Span[1]);
            break;
          case 3:
            for (auto &Op : Span)
              if (Op.Opcode == NdOp::STORE) {
                Op = LowOp{};
                Op.Opcode = NdOp::NOP;
                Op.Addr = I.Address;
              }
            break;
          case 4:
            Span.back().Opcode = NdOp::NOP;
            break;
          case 5:
            Span.front().Inputs[5].Offset ^= 1; // inactive metadata still binds
            break;
          case 6:
            Span.front().ConsumesX87Argument = true;
            break;
          case 7:
            ++C.Original.Fallthrough.Address;
            break;
          case 8:
            C.Original.NativeBytes[1] ^= 1;
            break;
          case 9:
            for (auto &Origin : R.Origins)
              if (Origin.NativeInstruction.Address == Base + 0x1002)
                ++Origin.NativeInstruction.Address;
            break;
          }
          C.ResidualOperationDigest = lowUndefinedOperationDigest(Span);
        }
    ASSERT_TRUE(Found);
    C.IdentityDigest = x64::returningCallDigest(C);
    // Do not invoke the outer receipt validator: reach the semantic native
    // continuation/expansion check with refreshed per-span checksum fields.
    auto Error = x64::validateReturningCalls(R, O);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST_F(PEFixedImageTest, ReturningCallPreparationNodeAndSealBoundsAreExact) {
  auto Loaded = load(machineExitFixture({0xff, 0xd0, 0xc3}));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto O = returningCallOptions(Base + 0x1000, Base + 0x1002);
  const auto Good = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  const auto run = [&](const auto &Options) {
    return specializeBinaryInterpreter(*Loaded, Base + 0x1000, Options);
  };
  const auto Empty = [](const SpecializationResult &R) {
    EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded) << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.Origins.empty());
    EXPECT_TRUE(R.ReturningCalls.empty());
    EXPECT_TRUE(R.ExternalExits.empty());
    EXPECT_FALSE(R.RawEnvironment);
  };
  const auto Check = [&](auto Member) {
    uint64_t Low = 0, High = O.*Member;
    while (Low < High) {
      auto Probe = O;
      Probe.*Member = Low + (High - Low) / 2;
      const auto R = run(Probe);
      if (R.complete())
        High = Probe.*Member;
      else {
        Empty(R);
        Low = Probe.*Member + 1;
      }
    }
    ASSERT_GT(Low, 0U);
    auto Exact = O;
    Exact.*Member = Low;
    const auto Result = run(Exact);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    ASSERT_EQ(Result.ReturningCalls.size(), 1U);
    --(Exact.*Member);
    Empty(run(Exact));
  };
  Check(&SpecializationOptions::MaxImagePreparationBytes);
  Check(&SpecializationOptions::MaxImagePreparationRecords);
  Check(&SpecializationOptions::MaxSymbolicNodes);
  Check(&SpecializationOptions::MaxRawEnvironmentResultBytes);
  Check(&SpecializationOptions::MaxRawEnvironmentResultRecords);
}
