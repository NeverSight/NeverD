//===- ReturningCallReplayJoinTests.inc - Recovery failure paths --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Actual-provider replay and unchanged provenance-join budget controls.
//===----------------------------------------------------------------------===//

class ReturningCallReadCounter : public ReturningCallProvider {
public:
  std::vector<va_t> Reads;

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    Reads.push_back(Cursor.Address);
    return Provider::instruction(Cursor);
  }

  size_t readsAt(va_t Address) const {
    return std::count(Reads.begin(), Reads.end(), Address);
  }
};

void expectNoReturningCallPublication(const SpecializationResult &R) {
  EXPECT_TRUE(R.Residual.Blocks.empty());
  EXPECT_TRUE(R.Origins.empty());
  EXPECT_TRUE(R.Reads.empty());
  EXPECT_TRUE(R.ReturningCalls.empty());
  EXPECT_TRUE(R.ExternalExits.empty());
  EXPECT_FALSE(R.RawEnvironment);
}

TEST(InterpreterSpecialization, ReturningCallReplayBudgetFailureDiscardsDraft) {
  ReturningCallReadCounter P;
  P.native(0x101, {0xff, 0xd7});
  P.add(0x103, {jump(0x200)});
  P.add(0x200, {op(NdOp::INDIR_BR, {}, {reg(x86reg::RAX)})});
  P.native(0x300, {0xc3});
  auto O = returningStateOptions(0x101, 0x300);
  O.DiscoverControlState = true;
  O.MaxChainedTransfers = 0;
  O.ControlRegisters = {{x86reg::RAX, 8}};
  O.MaxControlRefinements = 1;
  auto Prepared = P.prepare(O);
  ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
  const auto Run = [&](uint64_t Budget) {
    P.Reads.clear();
    auto Limited = O;
    Limited.MaxOperations = Budget;
    // The prepared domain binds budget policy too; use the actual allowance.
    if (auto Error = P.prepare(Limited)) {
      ADD_FAILURE() << llvm::toString(std::move(Error));
      return SpecializationResult{};
    }
    return specializeInterpreter(P, {0x101}, Limited);
  };
  const auto Baseline = Run(O.MaxOperations);
  RecordProperty("baseline_call_reads", std::to_string(P.readsAt(0x101)));
  RecordProperty("baseline_evaluations",
                 std::to_string(Baseline.NodeEvaluations));
  RecordProperty("baseline_refinements",
                 std::to_string(Baseline.ControlRefinements));
  RecordProperty("baseline_work", std::to_string(Baseline.EvaluatedOperations));
  RecordProperty("baseline_diagnostic", Baseline.Diagnostic);
  ASSERT_GE(P.readsAt(0x101), 2U) << Baseline.Diagnostic;
  expectNoReturningCallPublication(Baseline);
  // This acyclic graph has exactly one initial caller and one failing target
  // node. A second caller read while ControlRefinements is still zero is the
  // predecessor evaluator replay, before any fresh-graph retry is possible.
  uint64_t Low = 0, High = Baseline.EvaluatedOperations;
  while (Low < High) {
    const uint64_t Mid = Low + (High - Low) / 2;
    Run(Mid);
    if (P.readsAt(0x101) < 2)
      Low = Mid + 1;
    else
      High = Mid;
  }
  bool SawRegisterFailure = false, SawMemoryFailure = false;
  for (uint64_t Budget = Low; Budget <= Baseline.EvaluatedOperations;
       ++Budget) {
    const auto Failed = Run(Budget);
    if (P.readsAt(0x101) != 2 || Failed.ControlRefinements != 0)
      continue;
    const bool Register =
        Failed.Diagnostic.find("register preservation refused") !=
        std::string::npos;
    const bool Memory = Failed.Diagnostic.find("memory preservation refused") !=
                        std::string::npos;
    if (!Register && !Memory)
      continue;
    ASSERT_EQ(Failed.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_EQ(Failed.NodeEvaluations, 3U);
    EXPECT_GT(Failed.DiscoveryVisits, 0U);
    expectNoReturningCallPublication(Failed);
    if ((Register && !SawRegisterFailure) || (Memory && !SawMemoryFailure)) {
      const std::string Prefix = Register ? "register_" : "memory_";
      RecordProperty(Prefix + "budget", std::to_string(Budget));
      RecordProperty(Prefix + "work",
                     std::to_string(Failed.EvaluatedOperations));
      RecordProperty(Prefix + "evaluations",
                     std::to_string(Failed.NodeEvaluations));
      RecordProperty(Prefix + "refinements",
                     std::to_string(Failed.ControlRefinements));
      RecordProperty(Prefix + "discovery_visits",
                     std::to_string(Failed.DiscoveryVisits));
      RecordProperty(Prefix + "diagnostic", Failed.Diagnostic);
    }
    SawRegisterFailure |= Register;
    SawMemoryFailure |= Memory;
    if (SawRegisterFailure && SawMemoryFailure)
      break;
  }
  EXPECT_TRUE(SawRegisterFailure);
  EXPECT_TRUE(SawMemoryFailure);
}

TEST(InterpreterSpecialization, ReturningCallUnchangedJoinStillChargesScans) {
  // Independently counted safe-byte facts after the V1 call: ten non-stack
  // preserved GPRs (80), ten low-XMM ranges (160), FPCW (2), and the assigned
  // guest PC (8). RSP and DF have unsafe entry origins; new flags are fresh.
  constexpr uint64_t SafeBytes = 250;
  // A second edge adds one projection scan and four bounded merge scans.
  // COND_BR also reads one flag byte beyond the unconditional target input.
  constexpr uint64_t ExtraWork = 5 * SafeBytes + 1;
  std::array<uint64_t, 2> Work{};
  for (bool DuplicateEdge : {false, true}) {
    ReturningCallReadCounter P;
    P.native(0x101, {0xff, 0xd7});
    P.add(0x103, {jump(0x200)});
    if (DuplicateEdge)
      P.add(0x200,
            {op(NdOp::COND_BR, {}, {constant(0x300), reg(x86reg::ZF, 1)})},
            0x300);
    else
      P.add(0x200, {jump(0x300)});
    P.native(0x300, {0xc3});
    auto O = returningStateOptions(0x101, 0x300);
    O.MaxChainedTransfers = 0;
    auto Prepared = P.prepare(O);
    ASSERT_FALSE(bool(Prepared)) << llvm::toString(std::move(Prepared));
    const auto Good = specializeInterpreter(P, {0x101}, O);
    ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
    ASSERT_EQ(Good.ReturningCalls.size(), 1U);
    ASSERT_EQ(Good.ExternalExits.size(), 1U);
    EXPECT_EQ(Good.NodeEvaluations, 3U);
    EXPECT_EQ(Good.Contexts, 3U);
    EXPECT_EQ(P.readsAt(0x101), 1U);
    EXPECT_EQ(P.readsAt(0x200), 1U);
    EXPECT_EQ(P.readsAt(0x300), 1U);
    Work[DuplicateEdge] = Good.EvaluatedOperations;
    O.MaxOperations = Good.EvaluatedOperations;
    auto ExactPrepared = P.prepare(O);
    ASSERT_FALSE(bool(ExactPrepared))
        << llvm::toString(std::move(ExactPrepared));
    const auto Exact = specializeInterpreter(P, {0x101}, O);
    ASSERT_TRUE(Exact.complete()) << Exact.Diagnostic;
    --O.MaxOperations;
    auto ShortPrepared = P.prepare(O);
    ASSERT_FALSE(bool(ShortPrepared))
        << llvm::toString(std::move(ShortPrepared));
    const auto Short = specializeInterpreter(P, {0x101}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    expectNoReturningCallPublication(Short);
  }
  EXPECT_EQ(Work[1], Work[0] + ExtraWork);
}
