//===- ReturningCallStateEffectTests.inc - Roots and current identities --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Direct state observations of the exact private effect used by recovery.
/// Complements actual-provider tests; separate evaluator locals are not
/// arguments here and their preservation is checked through consumer probes.
//===----------------------------------------------------------------------===//

void returningStateSwapGS(SymState &S, RuntimeControlState &Controls,
                          const SpecializationOptions &O) {
  uint64_t Next = 0x1000;
  const auto Swap =
      op(NdOp::INTRINSIC, {}, {constant(uint64_t(Intrinsic::Swapgs), 2)});
  const auto T = x64::lowerRuntimeControlOperation(
      Swap, S, Controls, O, [&](uint16_t Bytes) {
        const auto V = NdVar::tmp(Next, Bytes);
        Next += Bytes;
        return V;
      });
  ASSERT_TRUE(T);
  ASSERT_TRUE(T->OutputState);
  SymExec E(S.context(), S);
  for (const auto &Op : T->Ops)
    ASSERT_EQ(E.step(Op), StepResult::Continue);
  auto Error =
      x64::commitRuntimeControlState(S, Controls, T->OutputState->first,
                                     E.operandValue(T->OutputState->second), O);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST(InterpreterSpecialization,
     ReturningCallEffectKeepsSegmentRootsAndCurrentViews) {
  for (bool Aliased : {false, true}) {
    SymContext C;
    SymState S(C);
    const auto Root = C.mkFreshVar(64, "frame");
    const auto GS = C.mkFreshVar(64, "original_active_gs");
    const auto FS = Aliased ? GS : C.mkFreshVar(64, "original_fs");
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FS));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, GS));
    auto O = returningStateOptions();
    O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
    O.X64LegacyGSState = true;
    O.X64MXCSRMask = 0xffff;
    auto Controls = x64::seedRuntimeControlState(C, S, O);
    ASSERT_EQ(Controls.Values.size(), 2U);
    if (Aliased)
      Controls.Values.back() = C.mkConcat(GS, GS);
    returningStateSwapGS(S, Controls, O);
    const auto Active =
        S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS);
    const auto ControlValues = Controls.Values;
    const auto Caller = C.mkAdd(Root, C.mkConst(64, uint64_t(-64)));
    S.write(SymSpace::Register, x86reg::RSP, C.mkSub(Caller, C.mkConst(64, 8)));
    const auto At = [&](int64_t Offset) {
      return C.mkAdd(Root, C.mkConst(64, uint64_t(Offset)));
    };
    const auto Word = C.mkFreshVar(64, "current_word");
    S.store(At(40), Word);
    S.store(At(43), C.mkConst(16, 0x7193));
    const auto Current = S.load(At(40), 8);
    const auto Overwritten = C.asConst(S.load(At(43), 2));
    ASSERT_TRUE(Overwritten);
    EXPECT_EQ(Overwritten->getZExtValue(), 0x7193U);
    S.store(At(-72), C.mkConst(64, 0x103));
    const auto Other = C.mkFreshVar(64, "other_region");
    const auto AbsoluteBefore = S.load(C.mkConst(64, 0x9000), 8);
    const auto OtherBefore = S.load(Other, 8);
    const auto TempBefore = S.read(SymSpace::Temporary, 0x5000, 8);
    auto Sibling = S;
    const auto SiblingHole = Sibling.load(At(80), 8);
    const auto Nodes = C.numNodes();
    const auto Policy = x64::returningCallTransition();
    ReturningCallStateEffect Effect;
    Effect.MaxWork = 100000;
    Effect.MaxNodes = Nodes + 4096;
    applyReturningCallState(S, Root, Caller, -32, 256, 0x103, Policy, Effect);
    ASSERT_EQ(Effect.Status, SpecializationStatus::Complete)
        << Effect.Diagnostic;
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS), FS);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), Active);
    EXPECT_EQ(Controls.Values,
              ControlValues); // separate local: structural noninterference
    EXPECT_EQ(S.read(SymSpace::Register, x86reg::RSP, 8), Caller);
    EXPECT_EQ(
        C.asConst(S.read(SymSpace::Register, x86reg::RIP, 8))->getZExtValue(),
        0x103U);
    uint64_t Work = 100000;
    const auto BeforeObservation = C.numNodes();
    const auto Ranges = S.materializedRegionRanges(Root, Work);
    ASSERT_TRUE(Ranges);
    EXPECT_EQ(C.numNodes(), BeforeObservation);
    ASSERT_EQ(Ranges->size(), 1U);
    EXPECT_EQ(Ranges->front().Offset, 40U);
    EXPECT_EQ(Ranges->front().Bytes, 8U);
    EXPECT_EQ(S.load(At(40), 8), Current);
    EXPECT_NE(S.load(At(80), 8), SiblingHole);
    EXPECT_EQ(Sibling.load(At(80), 8), SiblingHole);
    EXPECT_NE(S.load(C.mkConst(64, 0x9000), 8), AbsoluteBefore);
    EXPECT_NE(S.load(Other, 8), OtherBefore);
    EXPECT_NE(S.read(SymSpace::Temporary, 0x5000, 8), TempBefore);
    returningStateSwapGS(S, Controls, O);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), GS);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS), FS);
    EXPECT_EQ(C.mkExtract(Controls.Values.back(), 64, 64), Active);
    EXPECT_EQ(Controls.Values.front(), ControlValues.front());
  }
}

TEST(InterpreterSpecialization,
     ReturningCallEffectHasEveryJointFlagAndByteValue) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkFreshVar(64, "frame");
  ReturningCallStateEffect Effect;
  Effect.MaxWork = 100000;
  Effect.MaxNodes = 65536;
  applyReturningCallState(S, Root, Root, 32, 256, 0x103,
                          x64::returningCallTransition(), Effect);
  ASSERT_EQ(Effect.Status, SpecializationStatus::Complete) << Effect.Diagnostic;
  const uint64_t Flags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                            x86reg::ZF, x86reg::SF, x86reg::OF};
  for (unsigned Assignment = 0; Assignment < 64; ++Assignment) {
    solver::BitVectorSolver Solver(C);
    for (unsigned I = 0; I < std::size(Flags); ++I)
      ASSERT_TRUE(Solver.assertEqual(S.read(SymSpace::Register, Flags[I], 1),
                                     C.mkConst(8, (Assignment >> I) & 1)));
    EXPECT_EQ(Solver.check(), solver::SatResult::Sat) << Assignment;
  }
  const uint64_t Bytes[] = {x86reg::RAX, x86reg::XMM6 + 16, x86reg::K7 + 7,
                            x86reg::FPU_SW, x86reg::TMM7 + 1023};
  for (uint64_t Byte : Bytes) {
    const auto Value = S.read(SymSpace::Register, Byte, 1);
    ASSERT_EQ(C.width(Value), 8U);
    for (unsigned Sample = 0; Sample < 256; ++Sample) {
      solver::BitVectorSolver Solver(C);
      ASSERT_TRUE(Solver.assertEqual(Value, C.mkConst(8, Sample)));
      EXPECT_EQ(Solver.check(), solver::SatResult::Sat)
          << Byte << ':' << Sample;
    }
  }
}

TEST(InterpreterSpecialization, ReturningCallEffectEmptyStateHasExactLedgers) {
  // Independently counted V1 policy: eleven eight-byte GPR ranges (RSP
  // included), ten low-XMM ranges, DF and FPCW = 23 ranges, 251 bytes.
  // Output writes reserve 2*(8+8+6)=44 work and 13 nodes. Register forgetting
  // needs 1+23+2+3*251=779 work and 251 missing-byte nodes. Empty memory has
  // request/lookup/default plus the always-present absolute bank: 4 work.
  constexpr uint64_t ExactWork = 827;
  constexpr uint64_t AddedNodeAllowance = 264;
  bool SawRegisterFailure = false, SawLateMemoryFailure = false;
  for (uint64_t Budget = 0; Budget <= ExactWork; ++Budget) {
    SymContext C;
    SymState S(C);
    const auto Root = C.mkFreshVar(64, "frame");
    const auto Before = C.numNodes();
    ReturningCallStateEffect Effect;
    Effect.MaxWork = Budget;
    Effect.MaxNodes = Before + AddedNodeAllowance;
    applyReturningCallState(S, Root, Root, 32, 256, 0x103,
                            x64::returningCallTransition(), Effect);
    EXPECT_LE(Effect.WorkUsed, Budget);
    EXPECT_EQ(Effect.Status, Budget == ExactWork
                                 ? SpecializationStatus::Complete
                                 : SpecializationStatus::BudgetExceeded)
        << Budget << ':' << Effect.Diagnostic;
    SawRegisterFailure |= std::string_view(Effect.Diagnostic) ==
                          "returning-call register preservation refused";
    SawLateMemoryFailure |= std::string_view(Effect.Diagnostic) ==
                            "returning-call memory preservation refused";
    if (Budget == ExactWork) {
      EXPECT_EQ(Effect.WorkUsed, ExactWork);
      EXPECT_EQ(C.numNodes(), Before + AddedNodeAllowance);
      EXPECT_LE(C.numNodes(), Effect.MaxNodes);
    }
    // Every failed local composition dies here. No partial evaluator is
    // reused, and the actual-provider tests check nonpublication separately.
  }
  EXPECT_TRUE(SawRegisterFailure);
  EXPECT_TRUE(SawLateMemoryFailure);
  for (bool Exact : {false, true}) {
    SymContext C;
    SymState S(C);
    const auto Root = C.mkFreshVar(64, "frame");
    const auto Before = C.numNodes();
    ReturningCallStateEffect Effect;
    Effect.MaxWork = ExactWork;
    Effect.MaxNodes = Before + AddedNodeAllowance - (Exact ? 0 : 1);
    applyReturningCallState(S, Root, Root, 32, 256, 0x103,
                            x64::returningCallTransition(), Effect);
    EXPECT_EQ(Effect.Status, Exact ? SpecializationStatus::Complete
                                   : SpecializationStatus::BudgetExceeded);
    if (Exact) {
      EXPECT_EQ(C.numNodes(), Before + AddedNodeAllowance);
      EXPECT_LE(C.numNodes(), Effect.MaxNodes);
    } else {
      EXPECT_EQ(C.numNodes(), Before);
    }
  }
}
