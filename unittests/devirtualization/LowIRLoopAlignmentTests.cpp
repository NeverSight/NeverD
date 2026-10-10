//===- LowIRLoopAlignmentTests.cpp - Search paired loop phases ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/LowIRLoopInference.h"
#include "gtest/gtest.h"

#include "neverd/analysis/LowIRRefinement.h"

#include <set>

using namespace neverd;
using namespace neverd::analysis;

namespace {
NdVar n(uint64_t V, uint16_t Bytes = 8) { return NdVar::scalar(V, Bytes); }
NdVar r(uint64_t Offset) { return NdVar::reg(Offset, 8); }
NdVar t(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::tmp(Offset, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}

struct Program {
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  Program() {
    Function.Entry = 0x100;
    Contract.Frame = LowIRIndependenceFrame{{32, 8}, 0, 16};
    Contract.ReturnRegisters = {{0, 8}};
  }

  void block(int Id, va_t Address, std::vector<int> Successors) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = B.EndAddr = Address;
    B.Succs = std::move(Successors);
    Function.Blocks.push_back(std::move(B));
  }

  void instruction(llvm::ArrayRef<LowOp> Ops) {
    auto &B = Function.Blocks.back();
    LowInstructionBoundary IB;
    IB.Address = B.EndAddr++;
    IB.Size = 1;
    IB.FirstOp = B.Ops.size();
    IB.OpCount = Ops.size();
    for (auto O : Ops) {
      O.Addr = IB.Address;
      O.Seq = B.Ops.size() - IB.FirstOp;
      if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR) {
        IB.Control = LowInstructionControl::Branch;
        IB.ControlFlags = LowInstructionControlFlag::Branch;
        IB.Immediate = O.Inputs[0].Offset;
        if (O.Opcode == NdOp::COND_BR)
          IB.ControlFlags |= LowInstructionControlFlag::Conditional;
      } else if (O.Opcode == NdOp::RETURN) {
        IB.Control = LowInstructionControl::Return;
        IB.ControlFlags = LowInstructionControlFlag::Return;
      }
      B.Ops.push_back(O);
    }
    B.InstructionBoundaries.push_back(IB);
    LowInstructionUndefinedEffects Effects;
    Effects.Coverage = LowUndefinedCoverage::Complete;
    Effects.OpCount = IB.OpCount;
    Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(B.Ops).slice(IB.FirstOp, IB.OpCount));
    Records.push_back({B.Id, IB, std::move(Effects)});
  }

  void add(uint64_t Amount) {
    instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, t(8), {t(0)}),
                 op(NdOp::INT_ADD, t(16), {t(8), n(Amount)}),
                 op(NdOp::STORE, {}, {t(0), t(16)})});
  }
  void decrement() {
    instruction({op(NdOp::LOAD, t(0), {r(32)}),
                 op(NdOp::INT_SUB, t(8), {t(0), n(1)}),
                 op(NdOp::STORE, {}, {r(32), t(8)})});
  }
  void zeroBranch(va_t Address) {
    instruction({op(NdOp::LOAD, t(0), {r(32)}),
                 op(NdOp::INT_EQUAL, t(8, 1), {t(0), n(0)}),
                 op(NdOp::COND_BR, {}, {n(Address), t(8, 1)})});
  }
  void branch(va_t Address) {
    instruction({op(NdOp::BRANCH, {}, {n(Address)})});
  }
  LowIRLoopAlignmentResult check(const Program &B,
                                 const LowIRLoopAlignmentLimits &Limits = {},
                                 LowIRRefinementWitness Witness =
                                     LowIRRefinementWitness::LiftedBits) const {
    return inferAndCheckLowIRLoopRefinement(Function, Records, B.Function,
                                            Contract, Witness, Limits);
  }
};

// Independently authored arithmetic loops: both store a countdown and a sum
// in two frame words, and return 3 * count modulo 2^64. The rotated form tests
// its exit after decrementing and duplicates the final addition. Its preferred
// guarded-body cut is therefore one phase later than the ordinary loop's.
Program counterLoop(bool Rotated = false, uint64_t Amount = 3,
                    bool ClobberCounter = false) {
  Program P;
  P.block(0, 0x100, Rotated ? std::vector<int>{1, 4} : std::vector<int>{1});
  P.instruction({op(NdOp::STORE, {}, {r(32), r(8)}),
                 op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(0)})});
  if (Rotated)
    P.zeroBranch(0x500);
  else
    P.branch(0x200);
  P.block(1, 0x200, Rotated ? std::vector<int>{2, 3} : std::vector<int>{2, 4});
  if (Rotated)
    P.decrement();
  P.zeroBranch(Rotated ? 0x400 : 0x500);
  P.block(2, 0x300, {1});
  P.add(Amount);
  if (!Rotated)
    P.decrement();
  P.branch(0x200);
  if (Rotated) {
    P.block(3, 0x400, {4});
    P.add(Amount);
    P.branch(0x500);
  }
  P.block(4, 0x500, {});
  if (ClobberCounter)
    P.instruction({op(NdOp::STORE, {}, {r(32), n(17)})});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, AlignedLoopsGetFreshCertificate) {
  const auto A = counterLoop();
  const auto R = A.check(A);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_EQ(R.CandidateAttempts, 1U);
  EXPECT_EQ(R.PairingAttempts, 1U);
  ASSERT_TRUE(R.Refinement.Certificate);
  EXPECT_EQ(R.Refinement.Certificate->Scope,
            LowIRRefinementScope::InductiveLowIRLoops);
}

// The two implementations use different countdown storage, but expose the
// same complete final carrier words and frame. No entry count is bounded.
// A separate running sum needs a frame equality in addition to the counter.
Program relocatedCounter(bool Frame, uint64_t Counter, bool Sum,
                         unsigned Mutation = 0) {
  Program P;
  P.Contract.Frame->End = 32;
  P.Contract.ReturnRegisters = {{0, 8},  {8, 8},  {16, 8}, {24, 8},
                                {32, 8}, {64, 8}, {80, 8}, {96, 8}};
  P.Contract.PreservedRegisters = {{96, 8}};
  const auto Store = [&](uint64_t Offset, NdVar Value) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::STORE, {}, {t(0), Value})});
  };
  const auto LoadCount = [&] {
    if (Frame)
      P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Counter)}),
                     op(NdOp::LOAD, r(0), {t(0)})});
    else
      P.instruction({op(NdOp::COPY, r(0), {r(Counter)})});
  };
  const auto WriteCount = [&](NdVar Value) {
    if (Frame)
      Store(Counter, Value);
    else
      P.instruction({op(NdOp::COPY, r(Counter), {Value})});
  };
  P.block(0, 0x100, {1});
  P.instruction({op(NdOp::COPY, r(64), {n(0)}), op(NdOp::COPY, r(80), {n(0)})});
  for (uint64_t Offset : {0, 8, 16, 24})
    Store(Offset, n(0));
  WriteCount(r(8));
  P.branch(0x200);
  P.block(1, 0x200, {2, 3});
  LoadCount();
  P.instruction({op(NdOp::INT_EQUAL, t(0, 1), {r(0), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), t(0, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::INT_SUB, r(0), {r(0), n(Mutation == 4 ? 0 : 1)})});
  WriteCount(r(0));
  if (Sum)
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(16)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_ADD, t(16), {t(8), n(3)}),
                   op(NdOp::STORE, {}, {t(0), t(16)})});
  P.branch(0x200);
  P.block(3, 0x400, {});
  if (Mutation == 2)
    Store(24, n(7));
  if (Mutation == 3)
    P.instruction({op(NdOp::COPY, r(96), {n(0)})});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(16)}),
                 op(NdOp::LOAD, r(0), {t(0)}),
                 op(NdOp::INT_ADD, r(0), {r(0), n(Mutation == 1 ? 1 : 0)}),
                 op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

LowIRLoopAlignmentLimits rankPairingLimits() {
  LowIRLoopAlignmentLimits L;
  L.MaxRankPairingAttempts = 32;
  return L;
}

TEST(LowIRLoopAlignment, DirectRanksAlignRenamedCarriersAndFrameLayouts) {
  for (bool Frame : {false, true})
    for (bool Sum : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Frame << ":" << Sum);
      const auto A = relocatedCounter(Frame, Frame ? 0 : 64, Sum);
      const auto B = relocatedCounter(Frame, Frame ? 8 : 80, Sum);
      const auto Legacy = A.check(B);
      EXPECT_FALSE(Legacy.proved());
      EXPECT_FALSE(Legacy.Refinement.Certificate);
      EXPECT_EQ(Legacy.RankPairingAttempts, 0U);
      const auto R = A.check(B, rankPairingLimits());
      ASSERT_TRUE(R.proved())
          << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
      EXPECT_GT(R.RankPairingAttempts, 0U);
      EXPECT_GT(R.Refinement.LoopInitiations, 0U);
      EXPECT_GT(R.Refinement.LoopTransitions, 0U);
      EXPECT_GT(R.Refinement.RankingChecks, 0U);
      EXPECT_EQ(R.Refinement.Certificate->OriginalInstructions.size(),
                A.Records.size());
      EXPECT_EQ(R.Refinement.Certificate->Contract.ReturnRegisters.size(), 8U);
    }
}

TEST(LowIRLoopAlignment, DirectRanksCannotHideChangedObservationsOrProgress) {
  for (bool Frame : {false, true}) {
    const auto A = relocatedCounter(Frame, Frame ? 0 : 64, true);
    for (unsigned Mutation : {1, 2, 3, 4}) {
      SCOPED_TRACE(::testing::Message() << Frame << ":" << Mutation);
      const auto B = relocatedCounter(Frame, Frame ? 8 : 80, true, Mutation);
      const auto R = A.check(B, rankPairingLimits());
      EXPECT_FALSE(R.proved());
      EXPECT_FALSE(R.Refinement.Certificate);
    }
    auto Stale = A;
    Stale.Records.front().Effects.OpCount = 0;
    const auto R = Stale.check(relocatedCounter(Frame, Frame ? 8 : 80, true),
                               rankPairingLimits());
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
  }
}

TEST(LowIRLoopAlignment, DirectRanksKeepCumulativeBudgets) {
  const auto A = relocatedCounter(false, 64, true);
  const auto B = relocatedCounter(false, 80, true);
  auto Limits = rankPairingLimits();
  const auto Good = A.check(B, Limits);
  ASSERT_TRUE(Good.proved())
      << Good.Diagnostic << ": " << Good.LastCandidateDiagnostic;
  ASSERT_GT(Good.RankPairingAttempts, 1U);
  Limits.MaxSolverQueries = Good.SolverQueries;
  Limits.MaxSearchWork = Good.SearchWork;
  Limits.MaxPairingAttempts = Good.PairingAttempts;
  Limits.MaxRankPairingAttempts = Good.RankPairingAttempts;
  const auto Exact = A.check(B, Limits);
  ASSERT_TRUE(Exact.proved()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SolverQueries, Good.SolverQueries);
  EXPECT_EQ(Exact.SearchWork, Good.SearchWork);
  for (unsigned Budget : {0, 1, 2, 3}) {
    auto Short = Limits;
    if (Budget == 0)
      --Short.MaxSolverQueries;
    if (Budget == 1)
      --Short.MaxSearchWork;
    if (Budget == 2)
      --Short.MaxPairingAttempts;
    if (Budget == 3)
      --Short.MaxRankPairingAttempts;
    const auto R = A.check(B, Short);
    EXPECT_FALSE(R.proved()) << Budget;
    EXPECT_FALSE(R.Refinement.Certificate) << Budget;
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded) << Budget;
    EXPECT_LE(R.SearchWork, Short.MaxSearchWork);
    EXPECT_LE(R.SolverQueries, Short.MaxSolverQueries);
    EXPECT_LE(R.PairingAttempts, Short.MaxPairingAttempts);
    EXPECT_LE(R.RankPairingAttempts, Short.MaxRankPairingAttempts);
  }
}

// A nested loop with a separate reset phase: the inner counter is reset on
// one visit, and the outer counter decreases on the next. All frame words
// remain observable, including the phase flag and both counters.
Program phasedResetLoop(uint64_t Amount = 3, uint64_t OuterStep = 1,
                        bool TrailingLoop = false, bool LocalExitGuards = false,
                        bool ArithmeticDiamonds = false,
                        bool ReuseOuterForTrailing = false) {
  Program P;
  P.Contract.Frame->End = TrailingLoop && !ReuseOuterForTrailing ? 40 : 32;
  const uint64_t TrailingOffset = ReuseOuterForTrailing ? 0 : 32;
  const auto Load = [&](uint64_t Offset, NdVar Output) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::LOAD, Output, {t(0)})});
  };
  const auto Store = [&](uint64_t Offset, NdVar Value) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::STORE, {}, {t(0), Value})});
  };
  const auto Add = [&](uint64_t Offset, uint64_t Step) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_ADD, t(16), {t(8), n(Step)}),
                   op(NdOp::STORE, {}, {t(0), t(16)})});
  };
  const auto BeginAction = [&](int Id, va_t Address) {
    if (!ArithmeticDiamonds || Id != 4) {
      P.block(Id, Address, {1});
      return;
    }
    const int Decision = 100 + Id * 4, Join = Decision + 3;
    const auto At = [](int Block) { return (Block + 1) * 0x100; };
    P.block(Id, Address, {Decision});
    P.branch(At(Decision));
    P.block(Decision, At(Decision), {Decision + 1, Decision + 2});
    P.instruction({op(NdOp::INT_EQUAL, t(8, 1), {r(24), n(0)}),
                   op(NdOp::COND_BR, {}, {n(At(Decision + 1)), t(8, 1)})});
    for (int Arm : {1, 2}) {
      P.block(Decision + Arm, At(Decision + Arm), {Join});
      // Both paths preserve the observed sum modulo 2^64, with different
      // intermediate values before their common continuation.
      P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(16)}),
                     op(NdOp::LOAD, t(8), {t(0)}),
                     op(NdOp::INT_XOR, t(16), {t(8), n(Arm)}),
                     op(NdOp::STORE, {}, {t(0), t(16)})});
      P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(16)}),
                     op(NdOp::LOAD, t(8), {t(0)}),
                     op(NdOp::INT_XOR, t(16), {t(8), n(Arm)}),
                     op(NdOp::STORE, {}, {t(0), t(16)})});
      P.branch(At(Join));
    }
    P.block(Join, At(Join), {1});
  };
  P.block(0, 0x100, {1});
  Store(0, r(8));
  for (uint64_t Offset : {8, 16, 24})
    Store(Offset, n(0));
  if (TrailingLoop && !ReuseOuterForTrailing)
    Store(32, n(0));
  P.branch(0x200);
  P.block(1, 0x200,
          LocalExitGuards ? std::vector<int>{2} : std::vector<int>{2, 8});
  if (LocalExitGuards)
    P.branch(0x300);
  else
    P.zeroBranch(0x900);
  P.block(2, 0x300, {3, LocalExitGuards ? 22 : 7});
  P.instruction(
      {op(NdOp::INT_ADD, t(0), {r(32), n(24)}), op(NdOp::LOAD, t(8), {t(0)}),
       op(NdOp::INT_NOTEQUAL, t(16, 1), {t(8), n(0)}),
       op(NdOp::COND_BR, {}, {n(LocalExitGuards ? 0x1700 : 0x800), t(16, 1)})});
  P.block(3, 0x400,
          LocalExitGuards ? std::vector<int>{20, 21} : std::vector<int>{4, 5});
  P.instruction(
      {op(NdOp::INT_ADD, t(0), {r(32), n(8)}), op(NdOp::LOAD, t(8), {t(0)}),
       op(NdOp::INT_LESS, t(16, 1), {t(8), r(16)}),
       op(NdOp::COND_BR, {}, {n(LocalExitGuards ? 0x1500 : 0x500), t(16, 1)})});
  BeginAction(4, 0x500);
  Add(8, 1);
  Add(16, Amount);
  P.branch(0x200);
  BeginAction(5, 0x600);
  Store(8, n(0));
  Store(24, n(1));
  P.branch(0x200);
  BeginAction(7, 0x800);
  Add(0, -OuterStep);
  Store(24, n(0));
  P.branch(0x200);
  if (LocalExitGuards)
    for (const auto &[Id, Target] : {std::pair{20, 4}, {21, 5}, {22, 7}}) {
      P.block(Id, (Id + 1) * 0x100, {Target, 8});
      P.zeroBranch(0x900);
    }
  P.block(8, 0x900,
          TrailingLoop ? std::vector<int>{9, 10} : std::vector<int>{});
  if (TrailingLoop) {
    Store(TrailingOffset, r(24));
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(TrailingOffset)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_EQUAL, t(16, 1), {t(8), n(0)}),
                   op(NdOp::COND_BR, {}, {n(0xb00), t(16, 1)})});
    P.block(9, 0xa00, {9, 10});
    Add(TrailingOffset, -1);
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(TrailingOffset)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_NOTEQUAL, t(16, 1), {t(8), n(0)}),
                   op(NdOp::COND_BR, {}, {n(0xa00), t(16, 1)})});
    P.block(10, 0xb00, {});
  }
  Load(16, r(0));
  P.instruction({op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, SeparateResetAndProgressDiscoverPhaseCuts) {
  const auto P = phasedResetLoop();
  const auto R = P.check(P);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  ASSERT_TRUE(R.Refinement.Certificate);
  EXPECT_GT(R.Refinement.Certificate->LoopPlan->Cutpoints.size(), 1U);
  EXPECT_GT(R.Refinement.RankingChecks, 0U);
}

// Independently authored sequential countdowns reuse the first frame word.
// Each fresh input is unconstrained by the preceding loop's final counter.
Program sequentialCounterLoop(unsigned Loops = 2, bool NoProgress = false,
                              bool Repeat = false) {
  Program P;
  const auto At = [](unsigned Id) { return (Id + 1) * 0x100; };
  P.block(0, At(0), {1});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(0)})});
  P.branch(At(1));
  for (unsigned I = 0; I != Loops; ++I) {
    const unsigned Init = 1 + 3 * I, Header = Init + 1, Body = Init + 2;
    P.block(Init, At(Init), {int(Header)});
    P.instruction({op(NdOp::STORE, {}, {r(32), r(8 * (I + 1))})});
    P.branch(At(Header));
    P.block(Header, At(Header), {int(Body), int(Init + 3)});
    P.zeroBranch(At(Init + 3));
    P.block(Body, At(Body), {int(Header)});
    P.add(3 + 2 * I);
    if (!NoProgress || I + 1 != Loops)
      P.decrement();
    P.branch(At(Header));
  }
  const unsigned End = 1 + 3 * Loops;
  if (Repeat) {
    P.block(End, At(End), {1, int(End + 1)});
    P.instruction({op(NdOp::INT_NOTEQUAL, t(0, 1), {r(40), n(0)}),
                   op(NdOp::COND_BR, {}, {n(At(1)), t(0, 1)})});
  }
  P.block(Repeat ? End + 1 : End, At(Repeat ? End + 1 : End), {});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, LeadingPhasesAllowSequentialCounterReuse) {
  for (unsigned Loops : {2, 3}) {
    SCOPED_TRACE(Loops);
    const auto P = sequentialCounterLoop(Loops);
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(R.inferred()) << R.Diagnostic;
    ASSERT_EQ(R.Plan->Cutpoints.size(), Loops);
    std::set<uint64_t> Phases;
    for (const auto &Cut : R.Plan->Cutpoints) {
      ASSERT_EQ(Cut.Rank.size(), 2U);
      EXPECT_TRUE(Cut.Rank.front().isConst());
      EXPECT_EQ(Cut.Rank.front().Size, 8U);
      Phases.insert(Cut.Rank.front().Offset);
    }
    EXPECT_EQ(Phases.size(), Loops);
    const auto Proof = checkLowIRLoopRefinement(
        P.Function, P.Records, P.Function, P.Contract, *R.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    EXPECT_GT(Proof.RankingChecks, 0U);
    EXPECT_TRUE(P.check(P).proved());
  }
}

TEST(LowIRLoopAlignment, LeadingAndInnerPhasesComposeInOneRank) {
  const auto P = phasedResetLoop(3, 1, true, false, false, true);
  LowIRLoopInferenceLimits Limits;
  Limits.Execution.MaxSolverQueries = 16384;
  const auto R = detail::inferBranchArmLowIRLoopRefinementPlan(
      P.Function, P.Contract, Limits);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  ASSERT_EQ(R.Plan->Cutpoints.size(), 4U);
  std::set<uint64_t> Leading;
  for (const auto &Cut : R.Plan->Cutpoints) {
    ASSERT_GE(Cut.Rank.size(), 4U);
    ASSERT_TRUE(Cut.Rank.front().isConst());
    EXPECT_EQ(Cut.Rank.front().Size, 8U);
    Leading.insert(Cut.Rank.front().Offset);
  }
  EXPECT_EQ(Leading.size(), 2U);
  const auto Proof = checkLowIRLoopRefinement(P.Function, P.Records, P.Function,
                                              P.Contract, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
}

TEST(LowIRLoopAlignment, LeadingPhasesCannotHideNonProgressOrCyclicReset) {
  for (const auto &P : {sequentialCounterLoop(2, true),
                        sequentialCounterLoop(2, false, true)}) {
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    EXPECT_FALSE(R.inferred());
    EXPECT_FALSE(R.Plan);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::Unsupported) << R.Diagnostic;
    EXPECT_GE(R.RankCandidates, 2U) << R.Diagnostic;
    const auto Proof = P.check(P);
    EXPECT_FALSE(Proof.proved());
    EXPECT_FALSE(Proof.Refinement.Certificate);
  }
}

TEST(LowIRLoopAlignment, LeadingPhaseRanksAreIndependentlyRechecked) {
  const auto P = sequentialCounterLoop();
  const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  for (bool Reverse : {false, true}) {
    auto Plan = *R.Plan;
    for (auto &Cut : Plan.Cutpoints) {
      ASSERT_EQ(Cut.Rank.size(), 2U);
      ASSERT_TRUE(Cut.Rank.front().isConst());
      Cut.Rank.front().Offset = Reverse ? 1 - Cut.Rank.front().Offset : 0;
    }
    const auto Proof = checkLowIRLoopRefinement(P.Function, P.Records,
                                                P.Function, P.Contract, Plan);
    EXPECT_EQ(Proof.Status, LowIRRefinementStatus::Different);
    EXPECT_FALSE(Proof.Certificate);
    EXPECT_NE(Proof.Diagnostic.find("rank decrease"), std::string::npos)
        << Proof.Diagnostic;
  }
}

TEST(LowIRLoopAlignment, LeadingPhaseSearchChargesBothVariants) {
  const auto P = sequentialCounterLoop();
  const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  ASSERT_EQ(Good.RankCandidates, 2U);
  LowIRLoopInferenceLimits Limits;
  Limits.MaxRankCandidates = Good.RankCandidates;
  Limits.Execution.MaxSolverQueries = Good.SolverQueries;
  const auto Exact =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits);
  ASSERT_TRUE(Exact.inferred()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.RankCandidates, Good.RankCandidates);
  EXPECT_EQ(Exact.SolverQueries, Good.SolverQueries);
  for (bool ShortRanks : {false, true}) {
    auto Short = Limits;
    if (ShortRanks)
      --Short.MaxRankCandidates;
    else
      --Short.Execution.MaxSolverQueries;
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(R.Plan);
    EXPECT_LE(R.RankCandidates, Short.MaxRankCandidates);
    EXPECT_LE(R.SolverQueries, Short.Execution.MaxSolverQueries);
    if (ShortRanks) {
      EXPECT_EQ(R.RankCandidates, 1U);
      EXPECT_LT(R.SolverQueries, Good.SolverQueries);
    } else {
      EXPECT_EQ(R.SolverQueries, Short.Execution.MaxSolverQueries);
    }
  }
}

TEST(LowIRLoopAlignment, LeadingPhasesPreserveFinalStateAndOriginalEvidence) {
  const auto P = sequentialCounterLoop();
  const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  for (bool FrameWrite : {false, true}) {
    auto Wrong = P;
    Wrong.Function.Blocks.back().Ops.clear();
    Wrong.Function.Blocks.back().InstructionBoundaries.clear();
    Wrong.Function.Blocks.back().EndAddr =
        Wrong.Function.Blocks.back().StartAddr;
    if (FrameWrite)
      Wrong.instruction({op(NdOp::STORE, {}, {r(32), n(19)})});
    Wrong.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                       op(NdOp::LOAD, r(0), {t(0)}),
                       op(NdOp::RETURN, {}, {FrameWrite ? r(0) : n(0)})});
    const auto Proof = checkLowIRLoopRefinement(
        P.Function, P.Records, Wrong.Function, P.Contract, *R.Plan);
    EXPECT_EQ(Proof.Status, LowIRRefinementStatus::Different)
        << Proof.Diagnostic;
    EXPECT_FALSE(Proof.Certificate);
  }
  auto Records = P.Records;
  Records.front().Effects.Coverage = LowUndefinedCoverage::Missing;
  const auto Missing = checkLowIRLoopRefinement(P.Function, Records, P.Function,
                                                P.Contract, *R.Plan);
  EXPECT_EQ(Missing.Status, LowIRRefinementStatus::Unsupported);
  EXPECT_FALSE(Missing.Certificate);
}

// Independently authored partial-word arithmetic loops keep all outside bits.
struct CounterLane {
  unsigned Offset, Bytes;
  bool Register, Increment;
  unsigned WholeBytes = 8, FrameOffset = 0;
  llvm::endianness ByteOrder = llvm::endianness::little;
};
Program partialCounterLoop(CounterLane S, unsigned Loops = 2,
                           bool WholeGuard = false, bool NoProgress = false,
                           bool Bypass = false, bool Clobber = false) {
  Program P;
  P.Contract.ByteOrder = S.ByteOrder;
  P.Contract.Frame->Begin = S.FrameOffset;
  const auto At = [](unsigned Id) { return (Id + 1) * 0x100; };
  const auto Load = [&](unsigned Offset, unsigned Bytes) -> std::vector<LowOp> {
    if (S.Register)
      return {op(NdOp::COPY, t(8, Bytes), {NdVar::reg(56 + Offset, Bytes)})};
    return {op(NdOp::INT_ADD, t(0), {r(32), n(S.FrameOffset + Offset)}),
            op(NdOp::LOAD, t(8, Bytes), {t(0)})};
  };
  P.block(0, At(0), {1});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(0)})});
  P.branch(At(1));
  for (unsigned I = 0; I != Loops; ++I) {
    unsigned Init = 1 + 3 * I, Header = Init + 1, Body = Init + 2;
    P.block(Init, At(Init),
            Bypass && I + 1 == Loops ? std::vector<int>{int(Header), int(Body)}
                                     : std::vector<int>{int(Header)});
    if (S.Register)
      P.instruction({op(NdOp::COPY, r(56), {r(8 * (I + 1))})});
    else
      P.instruction(
          {op(NdOp::INT_ADD, t(0), {r(32), n(S.FrameOffset)}),
           op(NdOp::STORE, {}, {t(0), NdVar::reg(8 * (I + 1), S.WholeBytes)})});
    if (Bypass && I + 1 == Loops)
      P.instruction({op(NdOp::INT_NOTEQUAL, t(0, 1), {r(40), n(0)}),
                     op(NdOp::COND_BR, {}, {n(At(Body)), t(0, 1)})});
    else
      P.branch(At(Header));
    P.block(Header, At(Header), {int(Body), int(Init + 3)});
    unsigned GBytes = WholeGuard ? S.WholeBytes : S.Bytes,
             GOffset = WholeGuard ? 0 : S.Offset;
    auto Guard = Load(GOffset, GBytes);
    Guard.push_back(
        op(NdOp::INT_EQUAL, t(16, 1),
           {t(8, GBytes), n(S.Increment ? UINT64_MAX : 0, GBytes)}));
    Guard.push_back(op(NdOp::COND_BR, {}, {n(At(Init + 3)), t(16, 1)}));
    P.instruction(Guard);
    P.block(Body, At(Body), {int(Header)});
    P.add(3 + 2 * I);
    if (!NoProgress || I + 1 != Loops) {
      auto Step = Load(S.Offset, S.Bytes);
      Step.push_back(op(S.Increment ? NdOp::INT_ADD : NdOp::INT_SUB,
                        t(16, S.Bytes), {t(8, S.Bytes), n(1, S.Bytes)}));
      if (S.Register)
        Step.push_back(op(NdOp::COPY, NdVar::reg(56 + S.Offset, S.Bytes),
                          {t(16, S.Bytes)}));
      else
        Step.push_back(op(NdOp::STORE, {}, {t(0), t(16, S.Bytes)}));
      P.instruction(Step);
    }
    P.branch(At(Header));
  }
  unsigned End = 1 + 3 * Loops;
  P.block(End, At(End), {});
  if (S.Register)
    P.instruction({op(NdOp::STORE, {}, {r(32), r(56)})});
  if (Clobber) {
    unsigned NumericOffset = S.ByteOrder == llvm::endianness::big && !S.Register
                                 ? S.WholeBytes - S.Offset - S.Bytes
                                 : S.Offset;
    unsigned Bit = NumericOffset == 0 ? S.WholeBytes * 8 - 1 : 0;
    P.instruction(
        {op(NdOp::INT_ADD, t(0), {r(32), n(S.FrameOffset)}),
         op(NdOp::LOAD, t(8, S.WholeBytes), {t(0)}),
         op(NdOp::INT_XOR, t(16, S.WholeBytes),
            {t(8, S.WholeBytes), n(uint64_t{1} << Bit, S.WholeBytes)}),
         op(NdOp::STORE, {}, {t(0), t(16, S.WholeBytes)})});
  }
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, PartialCounterLanesPreserveFullState) {
  for (bool Register : {false, true})
    for (bool Increment : {false, true})
      for (const auto &[Offset, Bytes] :
           {std::pair{0U, 1U}, {0U, 2U}, {2U, 3U}, {7U, 1U}, {1U, 7U}}) {
        SCOPED_TRACE(::testing::Message() << Register << ':' << Increment << ':'
                                          << Offset << ':' << Bytes);
        const CounterLane Lane{Offset, Bytes, Register, Increment};
        const auto P = partialCounterLoop(Lane);
        const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
        ASSERT_TRUE(R.inferred()) << R.Diagnostic;
        const auto Proof = checkLowIRLoopRefinement(
            P.Function, P.Records, P.Function, P.Contract, *R.Plan);
        ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        auto Wrong = partialCounterLoop(Lane, 2, false, false, false, true);
        const auto Bad = checkLowIRLoopRefinement(
            P.Function, P.Records, Wrong.Function, P.Contract, *R.Plan);
        EXPECT_EQ(Bad.Status, LowIRRefinementStatus::Different)
            << Bad.Diagnostic;
        EXPECT_FALSE(Bad.Certificate);
      }
  const auto P = partialCounterLoop({0, 1, false, false});
  const auto Automatic = P.check(P);
  EXPECT_TRUE(Automatic.proved()) << Automatic.Diagnostic;
}

TEST(LowIRLoopAlignment, PartialCounterLanesRespectByteOrderAndFrameEnds) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool Increment : {false, true})
      for (bool Partial : {false, true}) {
        const CounterLane Lane{
            1,    1, false, Increment, Partial ? 3U : 8U, Partial ? 5U : 0U,
            Order};
        const auto P = partialCounterLoop(Lane);
        const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
        ASSERT_TRUE(R.inferred()) << R.Diagnostic;
        const auto Proof = checkLowIRLoopRefinement(
            P.Function, P.Records, P.Function, P.Contract, *R.Plan);
        ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        auto Wrong = partialCounterLoop(Lane, 2, false, false, false, true);
        const auto Bad = checkLowIRLoopRefinement(
            P.Function, P.Records, Wrong.Function, P.Contract, *R.Plan);
        EXPECT_EQ(Bad.Status, LowIRRefinementStatus::Different)
            << Bad.Diagnostic;
        EXPECT_FALSE(Bad.Certificate);
      }
}

TEST(LowIRLoopAlignment, PartialCounterLanesRejectNonProgressAndUnguardedWrap) {
  for (bool Increment : {false, true})
    for (bool WholeGuard : {false, true}) {
      const auto P = partialCounterLoop({0, 1, false, Increment}, 2, WholeGuard,
                                        !WholeGuard);
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::Unsupported)
          << R.Diagnostic;
      EXPECT_FALSE(R.Plan);
      EXPECT_GE(R.RankCandidates, 2U);
    }
}

TEST(LowIRLoopAlignment, PartialLaneGuardsCannotExcludeAdditionalEntrances) {
  const CounterLane Lane{0, 1, false, false};
  const auto P = partialCounterLoop(Lane);
  const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  // This added entry can arrive at the decrement with a zero lane. The loop
  // may still terminate after one wrap, but the old guarded plan is invalid.
  const auto Bypass = partialCounterLoop(Lane, 2, false, false, true);
  const auto Bad =
      checkLowIRLoopRefinement(Bypass.Function, Bypass.Records, Bypass.Function,
                               Bypass.Contract, *R.Plan);
  EXPECT_EQ(Bad.Status, LowIRRefinementStatus::Different) << Bad.Diagnostic;
  EXPECT_FALSE(Bad.Certificate);
}

TEST(LowIRLoopAlignment, PartialCounterLanesRespectInferenceBudgets) {
  const auto P = partialCounterLoop({2, 3, false, false});
  const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  LowIRLoopInferenceLimits Limits;
  Limits.MaxRankCandidates = Good.RankCandidates;
  Limits.Execution.MaxSolverQueries = Good.SolverQueries;
  const auto Exact =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits);
  ASSERT_TRUE(Exact.inferred()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SolverQueries, Good.SolverQueries);
  EXPECT_EQ(Exact.RankCandidates, Good.RankCandidates);
  for (bool ShortRanks : {false, true}) {
    auto Short = Limits;
    if (ShortRanks)
      --Short.MaxRankCandidates;
    else
      --Short.Execution.MaxSolverQueries;
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
    EXPECT_LE(R.SolverQueries, Short.Execution.MaxSolverQueries);
    EXPECT_LE(R.RankCandidates, Short.MaxRankCandidates);
  }
  Limits.Execution.MaxSymbolicNodes = 64;
  const auto Nodes =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits);
  EXPECT_EQ(Nodes.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_FALSE(Nodes.Plan);
}

TEST(LowIRLoopAlignment, SingleCutPartialCounterLanesKeepScalarSearch) {
  for (bool Register : {false, true})
    for (bool Increment : {false, true}) {
      const auto P = partialCounterLoop({2, 3, Register, Increment}, 1);
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      ASSERT_TRUE(R.inferred()) << R.Diagnostic;
      const auto Proof = checkLowIRLoopRefinement(
          P.Function, P.Records, P.Function, P.Contract, *R.Plan);
      EXPECT_TRUE(Proof.proved()) << Proof.Diagnostic;
    }
}

// The second byte's recurrence is hidden until the first byte becomes zero.
Program delayedLaneCounter() {
  Program P;
  P.block(0, 0x100, {1});
  P.instruction({op(NdOp::INT_AND, t(0), {r(8), n(0xffff)}),
                 op(NdOp::INT_OR, t(8), {t(0), n(0x10000)}),
                 op(NdOp::STORE, {}, {r(32), t(8)}),
                 op(NdOp::STORE, {}, {r(32), n(1, 1)}),
                 op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(0)})});
  P.branch(0x200);
  P.block(1, 0x200, {2, 5});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(1)}),
                 op(NdOp::LOAD, t(8, 1), {t(0)}),
                 op(NdOp::INT_EQUAL, t(16, 1), {t(8, 1), n(0, 1)}),
                 op(NdOp::COND_BR, {}, {n(0x600), t(16, 1)})});
  P.block(2, 0x300, {3, 4});
  P.instruction({op(NdOp::LOAD, t(0, 1), {r(32)}),
                 op(NdOp::INT_EQUAL, t(8, 1), {t(0, 1), n(0, 1)}),
                 op(NdOp::COND_BR, {}, {n(0x500), t(8, 1)})});
  P.block(3, 0x400, {1});
  P.instruction({op(NdOp::LOAD, t(0, 1), {r(32)}),
                 op(NdOp::INT_SUB, t(8, 1), {t(0, 1), n(1, 1)}),
                 op(NdOp::STORE, {}, {r(32), t(8, 1)})});
  P.branch(0x200);
  P.block(4, 0x500, {1});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(1)}),
                 op(NdOp::LOAD, t(8, 1), {t(0)}),
                 op(NdOp::INT_SUB, t(16, 1), {t(8, 1), n(1, 1)}),
                 op(NdOp::STORE, {}, {t(0), t(16, 1)})});
  P.branch(0x200);
  P.block(5, 0x600, {6});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(2)})});
  P.branch(0x700);
  P.block(6, 0x700, {7, 8});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, t(8), {t(0)}),
                 op(NdOp::INT_EQUAL, t(16, 1), {t(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x900), t(16, 1)})});
  P.block(7, 0x800, {6});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, t(8), {t(0)}),
                 op(NdOp::INT_SUB, t(16), {t(8), n(1)}),
                 op(NdOp::STORE, {}, {t(0), t(16)})});
  P.branch(0x700);
  P.block(8, 0x900, {});
  P.instruction({op(NdOp::LOAD, r(0), {r(32)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, KnownCounterDiscoversLaterPreservedLane) {
  const auto P = delayedLaneCounter();
  const va_t Cuts[] = {0x300, 0x800};
  const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  const auto Proof = checkLowIRLoopRefinement(P.Function, P.Records, P.Function,
                                              P.Contract, *R.Plan);
  EXPECT_TRUE(Proof.proved()) << Proof.Diagnostic;
}

TEST(LowIRLoopAlignment, InternalDiamondsDoNotSplitLoopPhases) {
  const auto A = phasedResetLoop(3, 1, false, true);
  const auto B = phasedResetLoop(3, 1, false, false, true);
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxCandidateAttempts = 3;
  Limits.CandidateInference.MaxCutpointAttempts = 3;
  Limits.CandidateInference.Execution.MaxSolverQueries = 16384;
  Limits.Proof.Execution.MaxSolverQueries = 16384;
  const auto Filtered = detail::inferLowIRLoopRefinementPlanFamily(
      B.Function, B.Contract, Limits.CandidateInference,
      detail::LowIRLoopCutFamily::FilteredBranchArms);
  ASSERT_TRUE(Filtered.inferred())
      << Filtered.Diagnostic << "; queries=" << Filtered.SolverQueries
      << "; ranks=" << Filtered.RankCandidates
      << "; widening=" << Filtered.WideningRounds;
  ASSERT_EQ(Filtered.Plan->Cutpoints.size(), 3U);
  const auto R = A.check(B, Limits);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic
                          << "; pairings=" << R.PairingAttempts;
  ASSERT_TRUE(R.Refinement.Certificate);
  EXPECT_EQ(R.Refinement.Certificate->LoopPlan->Cutpoints.size(), 3U);
  EXPECT_EQ(R.CandidateAttempts, 3U);
}

TEST(LowIRLoopAlignment, CrossFamiliesAlignRelocatedExitGuards) {
  const auto Shared = phasedResetLoop();
  const auto Local = phasedResetLoop(3, 1, false, true);
  const auto Default =
      inferLowIRLoopRefinementPlan(Local.Function, Local.Contract);
  ASSERT_TRUE(Default.inferred()) << Default.Diagnostic;
  ASSERT_EQ(Default.Plan->Cutpoints.size(), 3U);
  const auto Duplicate = detail::inferBranchArmLowIRLoopRefinementPlan(
      Local.Function, Local.Contract, {}, &*Default.Plan);
  EXPECT_FALSE(Duplicate.inferred());
  EXPECT_EQ(Duplicate.SolverQueries, 0U);
  EXPECT_EQ(Duplicate.Diagnostic, "branch-arm cuts duplicate previous plan");
  const auto SharedDefault =
      inferLowIRLoopRefinementPlan(Shared.Function, Shared.Contract);
  ASSERT_FALSE(SharedDefault.inferred());

  // The default original plan and branch-arm candidate plan are the only
  // available pair. Cross-family search must reach it within two candidate
  // attempts even though same-family pairing cannot produce a certificate.
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxCandidateAttempts = 2;
  const auto Forward = Local.check(Shared, Limits);
  ASSERT_TRUE(Forward.proved())
      << Forward.Diagnostic << ": " << Forward.LastCandidateDiagnostic;
  EXPECT_EQ(Forward.CandidateAttempts, 2U);
  EXPECT_TRUE(Shared.check(Local, Limits).proved());
}

enum class DiamondContinuation { Header, Local, ExitBypass, HeaderBypass };

Program
diamondCounter(uint64_t Amount = 3,
               DiamondContinuation Continuation = DiamondContinuation::Header) {
  Program P;
  P.block(0, 0x100, {1});
  P.instruction({op(NdOp::STORE, {}, {r(32), r(8)}),
                 op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(0)})});
  P.branch(0x200);
  P.block(1, 0x200, {2, 5});
  P.zeroBranch(0x600);
  P.block(2, 0x300, {3, 4});
  P.instruction({op(NdOp::INT_EQUAL, t(0, 1), {r(16), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), t(0, 1)})});
  for (int Id : {3, 4}) {
    const int Next = Continuation == DiamondContinuation::Header ? 1
                     : Id == 4 && Continuation != DiamondContinuation::Local
                         ? 6
                         : 7;
    P.block(Id, (Id + 1) * 0x100, {Next});
    P.add(Amount);
    P.decrement();
    P.branch((Next + 1) * 0x100);
  }
  if (Continuation == DiamondContinuation::ExitBypass ||
      Continuation == DiamondContinuation::HeaderBypass) {
    const int Bypass = Continuation == DiamondContinuation::ExitBypass ? 5 : 1;
    P.block(6, 0x700, {7, Bypass});
    P.instruction({op(NdOp::INT_EQUAL, t(0, 1), {r(24), n(0)}),
                   op(NdOp::COND_BR, {}, {n((Bypass + 1) * 0x100), t(0, 1)})});
  }
  if (Continuation != DiamondContinuation::Header) {
    P.block(7, 0x800, {1});
    P.branch(0x200);
  }
  P.block(5, 0x600, {});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, CrossFamiliesReuseCachedCandidatePlans) {
  const auto A = diamondCounter(3), B = diamondCounter(4);
  for (const auto *P : {&A, &B}) {
    const auto Default = inferLowIRLoopRefinementPlan(P->Function, P->Contract);
    const auto Branch = detail::inferBranchArmLowIRLoopRefinementPlan(
        P->Function, P->Contract, {});
    ASSERT_TRUE(Default.inferred()) << Default.Diagnostic;
    ASSERT_TRUE(Branch.inferred()) << Branch.Diagnostic;
    ASSERT_EQ(Default.Plan->Cutpoints.size(), 1U);
    ASSERT_EQ(Branch.Plan->Cutpoints.size(), 2U);
    for (const auto *Plan : {&*Default.Plan, &*Branch.Plan})
      ASSERT_TRUE(checkLowIRLoopRefinement(P->Function, P->Records, P->Function,
                                           P->Contract, *Plan)
                      .proved());
  }
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxCandidateAttempts = 2;
  const auto R = A.check(B, Limits);
  EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(R.Refinement.Certificate);
  EXPECT_EQ(R.CandidateAttempts, 2U);
  EXPECT_EQ(R.PairingAttempts, 3U);
  // Both same-family checks fail on the wrong sum. Cross-family checks then
  // reuse the two cached candidate plans and reject their cut counts before
  // the first singleton hits the candidate budget. Re-inference would stop
  // before reaching this diagnostic.
  EXPECT_EQ(R.LastCandidateDiagnostic, "loop alignment cut counts differ");
}

TEST(LowIRLoopAlignment, PhaseCutsRejectWrongResultsAndMissingProgress) {
  const auto P = phasedResetLoop();
  for (const auto &Wrong : {phasedResetLoop(4), phasedResetLoop(3, 0)}) {
    const auto R = P.check(Wrong);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
  }
}

TEST(LowIRLoopAlignment, FilteredCutsDistinguishLocalAndBoundaryJoins) {
  const auto Boundary = diamondCounter();
  const auto Local = diamondCounter(3, DiamondContinuation::Local);
  const auto B = detail::inferLowIRLoopRefinementPlanFamily(
      Boundary.Function, Boundary.Contract, {},
      detail::LowIRLoopCutFamily::FilteredBranchArms);
  ASSERT_TRUE(B.inferred()) << B.Diagnostic;
  ASSERT_EQ(B.Plan->Cutpoints.size(), 2U);
  EXPECT_TRUE(checkLowIRLoopRefinement(Boundary.Function, Boundary.Records,
                                       Boundary.Function, Boundary.Contract,
                                       *B.Plan)
                  .proved());
  const auto L = detail::inferLowIRLoopRefinementPlanFamily(
      Local.Function, Local.Contract, {},
      detail::LowIRLoopCutFamily::FilteredBranchArms);
  EXPECT_EQ(L.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_EQ(L.Diagnostic, "no cyclic branch-arm cutpoints");
  EXPECT_FALSE(L.Plan);
  EXPECT_EQ(L.SolverQueries, 0U);
  EXPECT_GT(L.CutSelectionWork, 0U);
}

TEST(LowIRLoopAlignment, ReachableJoinDoesNotHideExitOrBoundaryBypass) {
  for (auto Continuation :
       {DiamondContinuation::ExitBypass, DiamondContinuation::HeaderBypass}) {
    const auto P = diamondCounter(3, Continuation);
    LowIRLoopInferenceLimits Limits;
    Limits.MaxCutpointAttempts = 1;
    const auto R = detail::inferLowIRLoopRefinementPlanFamily(
        P.Function, P.Contract, Limits,
        detail::LowIRLoopCutFamily::FilteredBranchArms);
    // Both arms can reach the local join, but one can bypass it. Merely
    // intersecting reachable nodes would incorrectly drop these arm cuts.
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_EQ(R.Diagnostic, "loop inference cutpoint budget exhausted");
    EXPECT_FALSE(R.Plan);
    EXPECT_EQ(R.SolverQueries, 0U);
    EXPECT_GT(R.CutSelectionWork, 0U);
  }
}

TEST(LowIRLoopAlignment, CutSelectionWorkHasExactAndIndependentLimits) {
  const auto P = diamondCounter();
  const auto Infer = [&](const LowIRLoopInferenceLimits &Limits) {
    return detail::inferLowIRLoopRefinementPlanFamily(
        P.Function, P.Contract, Limits,
        detail::LowIRLoopCutFamily::FilteredBranchArms);
  };
  const auto Good = Infer({});
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  ASSERT_GT(Good.CutSelectionWork, 0U);
  LowIRLoopInferenceLimits Limits;
  Limits.MaxCutSelectionWork = Good.CutSelectionWork;
  const auto Exact = Infer(Limits);
  ASSERT_TRUE(Exact.inferred()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.CutSelectionWork, Limits.MaxCutSelectionWork);
  for (uint64_t Budget : {Good.CutSelectionWork - 1, uint64_t(0)}) {
    Limits.MaxCutSelectionWork = Budget;
    const auto Short = Infer(Limits);
    EXPECT_EQ(Short.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_EQ(Short.Diagnostic,
              "loop inference cut selection budget exhausted");
    EXPECT_FALSE(Short.Plan);
    EXPECT_EQ(Short.SolverQueries, 0U);
    EXPECT_LE(Short.CutSelectionWork, Budget);
  }
  // This optional analysis does not spend or require work in the two
  // existing selectors, even when its allowance is zero.
  for (auto Family : {detail::LowIRLoopCutFamily::Default,
                      detail::LowIRLoopCutFamily::BranchArms}) {
    const auto R = detail::inferLowIRLoopRefinementPlanFamily(
        P.Function, P.Contract, Limits, Family);
    ASSERT_TRUE(R.inferred()) << R.Diagnostic;
    EXPECT_EQ(R.CutSelectionWork, 0U);
  }
}

TEST(LowIRLoopAlignment, LaterBroadFamilyReusesSuccessfulFilteredPlan) {
  const auto A = phasedResetLoop(3, 1, false, false, true);
  const auto B = phasedResetLoop(4);
  LowIRLoopAlignmentLimits Limits;
  Limits.OriginalInference.MaxCutpointAttempts = 3;
  Limits.OriginalInference.Execution.MaxSolverQueries = 16384;
  Limits.CandidateInference.Execution.MaxSolverQueries = 16384;
  Limits.Proof.Execution.MaxSolverQueries = 16384;
  Limits.MaxCandidateAttempts = 3;
  const auto R = A.check(B, Limits);
  EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(R.Refinement.Certificate);
  EXPECT_EQ(R.CandidateAttempts, 3U);
  EXPECT_EQ(R.PairingAttempts, 6U);
  // The filtered candidate was inferred first. The later broad family has
  // the same cuts and must not repeat inference or retain a second copy.
  EXPECT_EQ(R.LastCandidateDiagnostic,
            "branch-arm cuts duplicate previous plan");
}

TEST(LowIRLoopAlignment, FilteredSearchKeepsCumulativeWorkAfterFailures) {
  const auto A = phasedResetLoop(3, 1, false, true);
  const auto B = phasedResetLoop(3, 1, false, false, true);
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxCandidateAttempts = 3;
  Limits.CandidateInference.MaxCutpointAttempts = 3;
  Limits.CandidateInference.Execution.MaxSolverQueries = 16384;
  Limits.Proof.Execution.MaxSolverQueries = 16384;
  const auto Good = A.check(B, Limits);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  Limits.MaxSearchWork = Good.SearchWork;
  const auto Exact = A.check(B, Limits);
  ASSERT_TRUE(Exact.proved()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SearchWork, Good.SearchWork);
  --Limits.MaxSearchWork;
  const auto Short = A.check(B, Limits);
  EXPECT_EQ(Short.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Refinement.Certificate);
  EXPECT_LE(Short.SearchWork, Limits.MaxSearchWork);

  // Both the duplicate original family and the failed candidate selector
  // spend their static allowance. Neither can reset the global work count.
  Limits.MaxSearchWork = 262144;
  Limits.OriginalInference.MaxCutSelectionWork = 1;
  Limits.CandidateInference.MaxCutSelectionWork = 1;
  const auto One = A.check(B, Limits);
  Limits.OriginalInference.MaxCutSelectionWork = 0;
  Limits.CandidateInference.MaxCutSelectionWork = 0;
  const auto Zero = A.check(B, Limits);
  EXPECT_EQ(One.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(Zero.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(One.Refinement.Certificate);
  EXPECT_FALSE(Zero.Refinement.Certificate);
  EXPECT_EQ(One.SearchWork, Zero.SearchWork + 2);
  EXPECT_EQ(One.SolverQueries, Zero.SolverQueries);
}

TEST(LowIRLoopAlignment, ExhaustedGraphBudgetStartsNoSymbolicInference) {
  const auto A = counterLoop();
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSearchWork = 2 * A.Function.Blocks.size();
  for (const auto &B : A.Function.Blocks)
    Limits.MaxSearchWork += 2 * B.Succs.size();
  const auto R = A.check(A, Limits);
  EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(R.Diagnostic, "loop alignment search work exhausted");
  EXPECT_EQ(R.SearchWork, Limits.MaxSearchWork);
  EXPECT_EQ(R.SolverQueries, 0U);
  EXPECT_EQ(R.CandidateAttempts, 0U);
  EXPECT_FALSE(R.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, PhaseFamiliesKeepExactCumulativeBudgets) {
  const auto P = phasedResetLoop();
  const auto Good = P.check(P);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSolverQueries = Good.SolverQueries;
  Limits.MaxCandidateAttempts = Good.CandidateAttempts;
  Limits.MaxPairingAttempts = Good.PairingAttempts;
  const auto Exact = P.check(P, Limits);
  ASSERT_TRUE(Exact.proved()) << Exact.Diagnostic;
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    auto Short = Limits;
    if (Kind == 0)
      --Short.MaxSolverQueries;
    else if (Kind == 1)
      --Short.MaxCandidateAttempts;
    else
      --Short.MaxPairingAttempts;
    const auto R = P.check(P, Short);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded)
        << R.Diagnostic;
    EXPECT_FALSE(R.Refinement.Certificate);
  }
}

TEST(LowIRLoopAlignment, BranchArmsKeepPhasesAndCompleteOtherCycleCoverage) {
  for (auto Family : {detail::LowIRLoopCutFamily::BranchArms,
                      detail::LowIRLoopCutFamily::FilteredBranchArms}) {
    SCOPED_TRACE(static_cast<int>(Family));
    for (bool Trailing : {false, true}) {
      SCOPED_TRACE(Trailing);
      const auto P = phasedResetLoop(3, 1, Trailing);
      LowIRLoopInferenceLimits Limits;
      Limits.Execution.MaxSolverQueries = 16384;
      const auto R = detail::inferLowIRLoopRefinementPlanFamily(
          P.Function, P.Contract, Limits, Family);
      ASSERT_TRUE(R.inferred()) << R.Diagnostic;
      std::set<va_t> Expected{0x500, 0x600, 0x800};
      if (Trailing)
        Expected.insert(0xa00);
      std::set<va_t> Actual;
      for (const auto &C : R.Plan->Cutpoints)
        Actual.insert(C.OriginalAddress);
      EXPECT_EQ(Actual, Expected);
      const auto Proof = checkLowIRLoopRefinement(
          P.Function, P.Records, P.Function, P.Contract, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      auto Short = Limits;
      Short.MaxCutpointAttempts = Expected.size() - 1;
      const auto Rejected = detail::inferLowIRLoopRefinementPlanFamily(
          P.Function, P.Contract, Short, Family);
      EXPECT_EQ(Rejected.Status, LowIRLoopInferenceStatus::BudgetExceeded);
      EXPECT_FALSE(Rejected.Plan);
    }
  }
}

TEST(LowIRLoopAlignment, EmptyAndDuplicateBranchFamiliesDoNoSymbolicSearch) {
  Program Empty;
  Empty.block(0, 0x100, {});
  Empty.instruction({op(NdOp::RETURN, {}, {n(0)})});
  const auto E = detail::inferBranchArmLowIRLoopRefinementPlan(
      Empty.Function, Empty.Contract, {});
  EXPECT_EQ(E.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(E.Plan);
  EXPECT_EQ(E.SolverQueries, 0U);
  const auto P = counterLoop();
  const auto Default = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Default.inferred());
  const auto Duplicate = detail::inferBranchArmLowIRLoopRefinementPlan(
      P.Function, P.Contract, {}, &*Default.Plan);
  EXPECT_EQ(Duplicate.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Duplicate.Plan);
  EXPECT_EQ(Duplicate.SolverQueries, 0U);
}

TEST(LowIRLoopAlignment, RetainedPlansShareOneMetadataPool) {
  const auto P = counterLoop();
  const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Inferred.inferred());
  uint64_t Size = Inferred.Plan->Cutpoints.size();
  for (const auto &C : Inferred.Plan->Cutpoints)
    Size += C.Inputs.size() + C.Expressions.size() + C.OriginalState.size() +
            C.CandidateState.size() + C.Rank.size();
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxMetadata = Size * 2 - 1;
  Limits.MaxCandidateAttempts = 1;
  ASSERT_LE(Size, Limits.MaxMetadata);
  const auto R = P.check(P, Limits);
  EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(R.PairingAttempts, 0U);
  EXPECT_FALSE(R.Refinement.Certificate);
  EXPECT_EQ(R.LastCandidateDiagnostic,
            "loop alignment metadata budget exhausted");
}

TEST(LowIRLoopAlignment, PhaseFamiliesRetainOriginalEvidenceAndWitness) {
  auto A = phasedResetLoop();
  const auto B = A;
  A.Records.front().Effects.Coverage = LowUndefinedCoverage::Missing;
  const auto Missing = A.check(B);
  EXPECT_FALSE(Missing.proved());
  EXPECT_FALSE(Missing.Refinement.Certificate);
  A.Records.front().Effects.Coverage = LowUndefinedCoverage::Complete;
  A.Records.front().Effects.Effects.push_back({0, r(8), 0, 1, {}});
  const auto Lifted = A.check(B);
  ASSERT_TRUE(Lifted.proved())
      << Lifted.Diagnostic << ": " << Lifted.LastCandidateDiagnostic;
  EXPECT_GT(Lifted.Refinement.Producers, 0U);
  const auto Zero = A.check(B, {}, LowIRRefinementWitness::ZeroBits);
  EXPECT_FALSE(Zero.proved());
  EXPECT_FALSE(Zero.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, AlternativeCandidatePhaseEstablishesRelation) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto Left = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  const auto Right = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  ASSERT_TRUE(checkLowIRLoopRefinement(A.Function, A.Records, A.Function,
                                       A.Contract, *Left.Plan)
                  .proved());
  ASSERT_TRUE(checkLowIRLoopRefinement(B.Function, B.Records, B.Function,
                                       A.Contract, *Right.Plan)
                  .proved());
  LowIRLoopAlignmentLimits FirstOnly;
  FirstOnly.MaxCandidateAttempts = 1;
  const auto Rejected = A.check(B, FirstOnly);
  EXPECT_FALSE(Rejected.proved());
  EXPECT_FALSE(Rejected.Refinement.Certificate);
  EXPECT_EQ(Rejected.Refinement.Status, LowIRRefinementStatus::Different);
  const auto R = A.check(B);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_GT(R.CandidateAttempts, 1U);
  EXPECT_GT(R.PairingAttempts, 1U);
  const auto &Certificate = *R.Refinement.Certificate;
  EXPECT_TRUE(checkLowIRLoopRefinement(A.Function, A.Records, B.Function,
                                       A.Contract, *Certificate.LoopPlan,
                                       Certificate.Witness, Certificate.Limits)
                  .proved());
}
TEST(LowIRLoopAlignment, WrongResultAndFrameWritesCannotGetCertificates) {
  const auto A = counterLoop();
  for (const auto &B : {counterLoop(true, 4), counterLoop(true, 3, true)}) {
    const auto R = A.check(B);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.LastCandidateDiagnostic.empty());
  }
}

TEST(LowIRLoopAlignment, OriginalAuditedRecordsRemainMandatory) {
  const auto B = counterLoop(true);
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto A = counterLoop();
    if (Mutation == 0)
      A.Records.front().Effects.Coverage = LowUndefinedCoverage::Missing;
    else if (Mutation == 1)
      A.Records.front().Effects.OperationDigest = "stale";
    else
      A.Records.pop_back();
    const auto R = A.check(B);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_EQ(R.Status, Mutation == 1 ? LowIRLoopAlignmentStatus::Invalid
                                      : LowIRLoopAlignmentStatus::Unsupported)
        << R.Diagnostic;
    EXPECT_EQ(R.Refinement.Status, Mutation == 1
                                       ? LowIRRefinementStatus::Invalid
                                       : LowIRRefinementStatus::Unsupported);
  }
}

TEST(LowIRLoopAlignment, OriginalUndefinedWitnessIsNotReplacedBySelfInference) {
  auto A = counterLoop();
  const auto B = counterLoop(true);
  A.Records.front().Effects.Effects.push_back({0, r(8), 0, 1, {}});
  const auto Lifted = A.check(B);
  ASSERT_TRUE(Lifted.proved())
      << Lifted.Diagnostic << ": " << Lifted.LastCandidateDiagnostic;
  EXPECT_EQ(Lifted.Refinement.Certificate->Witness,
            LowIRRefinementWitness::LiftedBits);
  EXPECT_GT(Lifted.Refinement.Producers, 0U);
  const auto Zero = A.check(B, {}, LowIRRefinementWitness::ZeroBits);
  EXPECT_FALSE(Zero.proved());
  EXPECT_FALSE(Zero.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, QueriesRemainChargedAcrossFailedAttempts) {
  const auto A = counterLoop(), B = counterLoop(true);
  LowIRLoopAlignmentLimits FirstOnly;
  FirstOnly.MaxCandidateAttempts = 1;
  const auto First = A.check(B, FirstOnly);
  ASSERT_EQ(First.PairingAttempts, 1U);
  ASSERT_GT(First.SolverQueries, First.Refinement.SolverQueries);
  LowIRLoopAlignmentLimits Limited;
  Limited.MaxSolverQueries = First.SolverQueries + 1;
  const auto Exhausted = A.check(B, Limited);
  EXPECT_FALSE(Exhausted.proved());
  EXPECT_FALSE(Exhausted.Refinement.Certificate);
  EXPECT_EQ(Exhausted.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(Exhausted.SolverQueries, Limited.MaxSolverQueries);
  // The two duplicate branch families consume attempts but no solver query;
  // the following singleton exhausts the remaining shared query allowance.
  EXPECT_EQ(Exhausted.CandidateAttempts, 4U);
  EXPECT_NE(Exhausted.Diagnostic.find("total query budget"), std::string::npos);
  EXPECT_FALSE(Exhausted.LastCandidateDiagnostic.empty());
}

TEST(LowIRLoopAlignment, ExactTotalBudgetCanCompleteTheFinalProof) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto Baseline = A.check(B);
  ASSERT_TRUE(Baseline.proved());
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSolverQueries = Baseline.SolverQueries;
  const auto Exact = A.check(B, Limits);
  EXPECT_TRUE(Exact.proved()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SolverQueries, Limits.MaxSolverQueries);
  --Limits.MaxSolverQueries;
  const auto Short = A.check(B, Limits);
  EXPECT_EQ(Short.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Refinement.Certificate);
  EXPECT_EQ(Short.SolverQueries, Limits.MaxSolverQueries);
}

TEST(LowIRLoopAlignment, PerAttemptQueryExhaustionCanTryAnotherCut) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto Default = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  const auto Alternative =
      inferLowIRLoopRefinementPlan(B.Function, A.Contract, {}, {0x200});
  ASSERT_TRUE(Default.inferred());
  ASSERT_TRUE(Alternative.inferred());
  ASSERT_LT(Alternative.SolverQueries, Default.SolverQueries);
  LowIRLoopAlignmentLimits Limits;
  Limits.CandidateInference.Execution.MaxSolverQueries =
      Alternative.SolverQueries;
  const auto R = A.check(B, Limits);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_GT(R.CandidateAttempts, 1U);
  EXPECT_GT(R.SolverQueries, Alternative.SolverQueries);
}

TEST(LowIRLoopAlignment, SearchLimitsCannotYieldPartialSuccess) {
  const auto A = counterLoop(), B = counterLoop(true);
  for (unsigned Budget = 0; Budget != 8; ++Budget) {
    LowIRLoopAlignmentLimits L;
    switch (Budget) {
    case 0:
      L.MaxSolverQueries = 0;
      break;
    case 1:
      L.MaxSearchWork = 1;
      break;
    case 2:
      L.MaxCandidateAttempts = 0;
      break;
    case 3:
      L.MaxPairingAttempts = 0;
      break;
    case 4:
      L.MaxMetadata = 0;
      break;
    case 5:
      L.MaxCuts = 0;
      break;
    case 6:
      L.OriginalInference.Execution.MaxSolverQueries = 0;
      break;
    case 7:
      L.Proof.Execution.MaxSolverQueries = 0;
      break;
    }
    const auto R = A.check(B, L);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded)
        << Budget << ": " << R.Diagnostic;
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_LE(R.SolverQueries, L.MaxSolverQueries);
    EXPECT_LE(R.SearchWork, L.MaxSearchWork);
  }
}

TEST(LowIRLoopAlignment, MalformedGraphsAreRefusedBeforeSearch) {
  const auto A = counterLoop();
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto B = counterLoop(true);
    switch (Mutation) {
    case 0:
      B.Function.Blocks.back().Id = B.Function.Blocks.front().Id;
      break;
    case 1:
      B.Function.Blocks.back().StartAddr = B.Function.Entry;
      break;
    case 2:
      B.Function.Blocks.front().Succs.push_back(999);
      break;
    case 3:
      B.Function.Blocks.front().Succs.push_back(1);
      break;
    case 4:
      B.Function.Entry = 999;
      break;
    }
    const auto R = A.check(B);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::Invalid) << R.Diagnostic;
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_EQ(R.SolverQueries, 0U);
  }
}

TEST(LowIRLoopAlignment, NondecreasingAndWrappingCountersRemainUnproved) {
  const auto A = counterLoop();
  for (uint64_t Step : {uint64_t{0}, ~uint64_t{0}}) {
    auto B = counterLoop(true);
    B.Function.Blocks[1].Ops[1].Inputs[1] = n(Step);
    LowIRLoopAlignmentLimits Limits;
    Limits.CandidateInference.Execution.MaxSolverQueries = 512;
    const auto R = A.check(B, Limits);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_NE(R.Status, LowIRLoopAlignmentStatus::Invalid);
  }
}

Program alternativeLoops(bool ReverseAddresses) {
  Program P;
  P.block(0, 0x100, {1, 3});
  P.instruction(
      {op(NdOp::STORE, {}, {r(32), r(8)}),
       op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
       op(NdOp::STORE, {}, {t(0), n(0)}),
       op(NdOp::INT_EQUAL, t(8, 1), {r(16), n(0)}),
       op(NdOp::COND_BR, {}, {n(ReverseAddresses ? 0x200 : 0x400), t(8, 1)})});
  for (unsigned I = 0; I != 2; ++I) {
    const unsigned Header = 1 + I * 2;
    const va_t Address = 0x200 + I * 0x200;
    P.block(Header, Address, {int(Header + 1), 5});
    P.zeroBranch(0x600);
    P.block(Header + 1, Address + 0x100, {int(Header)});
    P.add((I == 0) != ReverseAddresses ? 3 : 5);
    P.decrement();
    P.branch(Address);
  }
  P.block(5, 0x600, {});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, MultipleCutsUseCheckedBoundedPermutations) {
  const auto A = alternativeLoops(false), B = alternativeLoops(true);
  const auto R = A.check(B);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  ASSERT_EQ(R.Refinement.Certificate->LoopPlan->Cutpoints.size(), 2U);
  EXPECT_EQ(R.CandidateAttempts, 1U);
  EXPECT_GT(R.PairingAttempts, 1U);
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxPairingAttempts = 1;
  const auto Exhausted = A.check(B, Limits);
  EXPECT_EQ(Exhausted.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Exhausted.Refinement.Certificate);
  EXPECT_EQ(Exhausted.PairingAttempts, 1U);
  Limits.MaxPairingAttempts = 128;
  Limits.MaxCuts = 1;
  const auto TooMany = A.check(B, Limits);
  EXPECT_EQ(TooMany.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(TooMany.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, SearchWorkIncludesMatchingAndFailedAttempts) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto R = A.check(B);
  ASSERT_TRUE(R.proved());
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSearchWork = R.SearchWork;
  const auto Exact = A.check(B, Limits);
  EXPECT_TRUE(Exact.proved()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SearchWork, Limits.MaxSearchWork);
  --Limits.MaxSearchWork;
  const auto Exhausted = A.check(B, Limits);
  EXPECT_EQ(Exhausted.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Exhausted.Refinement.Certificate);
  EXPECT_LE(Exhausted.SearchWork, Limits.MaxSearchWork);
}
Program boundedFrameCounter(uint64_t Offset, uint16_t Bytes,
                            bool Wrong = false) {
  Program P;
  P.Contract.Frame->End = 12;
  P.Contract.ReturnRegisters = {{0, 8}, {8, 8}};
  const auto At = op(NdOp::INT_ADD, t(16), {r(32), n(Offset)});
  P.block(0, 0x100, {1});
  P.instruction({op(NdOp::STORE, {}, {r(32), n(0)}),
                 op(NdOp::INT_ADD, t(16), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(16), n(0, 4)})});
  P.instruction({At, op(NdOp::STORE, {}, {t(16), NdVar::reg(8, Bytes)})});
  P.branch(0x200);
  P.block(1, 0x200, {2, 3});
  P.instruction({At, op(NdOp::LOAD, t(0, Bytes), {t(16)}),
                 op(NdOp::INT_EQUAL, t(8, 1), {t(0, Bytes), n(0, Bytes)}),
                 op(NdOp::COND_BR, {}, {n(0x400), t(8, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({At, op(NdOp::LOAD, t(0, Bytes), {t(16)}),
                 op(NdOp::INT_SUB, t(8, Bytes), {t(0, Bytes), n(1, Bytes)}),
                 op(NdOp::STORE, {}, {t(16), t(8, Bytes)})});
  P.branch(0x200);
  P.block(3, 0x400, {});
  P.instruction(
      {op(NdOp::COPY, r(0), {n(Wrong ? 1 : 0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, DirectRanksChargeDuplicateProposalsBeforeSkipping) {
  const auto A = boundedFrameCounter(0, 8);
  const auto B = boundedFrameCounter(0, 8, true);
  const auto Left = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  const auto Right = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  uint64_t Metadata = 0;
  for (const auto *Plan : {&*Left.Plan, &*Right.Plan}) {
    ASSERT_EQ(Plan->Cutpoints.size(), 1U);
    const auto &C = Plan->Cutpoints.front();
    ASSERT_EQ(C.Rank.size(), 1U);
    unsigned CurrentFrames = 0;
    for (const auto &I : C.Inputs)
      if (I.Side == LowIRLoopSide::Original &&
          I.Location.Space == LowIRLoopSpace::Frame) {
        ++CurrentFrames;
        EXPECT_EQ(I.Location.Offset, 0U);
        EXPECT_EQ(I.Location.Bytes, 8U);
        EXPECT_EQ(I.Temporary, C.Rank.front());
      }
    ASSERT_EQ(CurrentFrames, 1U);
    Metadata += 1 + C.Inputs.size() + C.Expressions.size() +
                C.OriginalState.size() + C.CandidateState.size() +
                C.Rank.size() + C.OriginalGuards.size() +
                C.CandidateGuards.size();
  }
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxRankPairingAttempts = 2;
  Limits.MaxPairingAttempts = 1;
  const auto Before = A.check(B, Limits);
  Limits.MaxPairingAttempts = 2;
  const auto After = A.check(B, Limits);
  for (const auto *R : {&Before, &After}) {
    ASSERT_EQ(R->Status, LowIRLoopAlignmentStatus::BudgetExceeded);
    EXPECT_EQ(R->Diagnostic, "loop alignment pairing budget exhausted");
    EXPECT_EQ(R->Refinement.Status, LowIRRefinementStatus::Different);
    EXPECT_FALSE(R->Refinement.Certificate);
  }
  EXPECT_EQ(Before.RankPairingAttempts, 0U);
  EXPECT_EQ(After.RankPairingAttempts, 1U);
  EXPECT_EQ(After.LastCandidateDiagnostic, "duplicate loop input pairing");
  EXPECT_EQ(After.SolverQueries, Before.SolverQueries);
  // The one direct frame binding duplicates the ordinary proposal. Count
  // both complete input scans, two pairing vectors and bindings, and the
  // component/cut/set comparisons independently of the returned work total.
  const uint64_t AddedWork = Metadata + 7 +
                             2 * Left.Plan->Cutpoints[0].Inputs.size() +
                             2 * Right.Plan->Cutpoints[0].Inputs.size();
  EXPECT_EQ(After.SearchWork - Before.SearchWork, AddedWork);
  Limits.MaxSearchWork = Before.SearchWork + AddedWork;
  const auto ExactWork = A.check(B, Limits);
  EXPECT_EQ(ExactWork.LastCandidateDiagnostic, "duplicate loop input pairing");
  EXPECT_EQ(ExactWork.Diagnostic, "loop alignment pairing budget exhausted");
  EXPECT_EQ(ExactWork.SearchWork, Limits.MaxSearchWork);
  --Limits.MaxSearchWork;
  const auto ShortWork = A.check(B, Limits);
  EXPECT_EQ(ShortWork.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(ShortWork.Diagnostic, "loop alignment search work exhausted");
  EXPECT_EQ(ShortWork.RankPairingAttempts, 1U);
  EXPECT_EQ(ShortWork.SolverQueries, Before.SolverQueries);
  EXPECT_FALSE(ShortWork.Refinement.Certificate);
  Limits.MaxSearchWork = LowIRLoopAlignmentLimits{}.MaxSearchWork;
  Limits.MaxMetadata = Metadata + 4;
  const auto Exact = A.check(B, Limits);
  EXPECT_EQ(Exact.LastCandidateDiagnostic, "duplicate loop input pairing");
  EXPECT_EQ(Exact.SearchWork, After.SearchWork);
  --Limits.MaxMetadata;
  const auto Short = A.check(B, Limits);
  EXPECT_EQ(Short.LastCandidateDiagnostic,
            "loop alignment metadata budget exhausted");
  EXPECT_EQ(Short.RankPairingAttempts, 1U);
  EXPECT_EQ(Short.PairingAttempts, 2U);
  EXPECT_EQ(Short.SolverQueries, Before.SolverQueries);
  EXPECT_EQ(Short.Refinement.Status, LowIRRefinementStatus::Different);
  EXPECT_FALSE(Short.Refinement.Certificate);
}

TEST(LowIRLoopAlignment,
     DirectRanksKeepLaterOrdinaryPairingsAfterOptionalLimit) {
  const auto A = alternativeLoops(false), B = alternativeLoops(true);
  auto Limits = rankPairingLimits();
  Limits.MaxRankPairingAttempts = 1;
  const auto R = A.check(B, Limits);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_EQ(R.RankPairingAttempts, 1U);
  EXPECT_GT(R.PairingAttempts, 2U);
}

TEST(LowIRLoopAlignment, DirectRanksRejectDifferentActualFrameInputWidths) {
  const auto A = boundedFrameCounter(0, 8);
  const auto B = boundedFrameCounter(8, 4);
  const auto Left = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  const auto Right = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  for (unsigned Side : {0, 1}) {
    const auto &Plan = Side ? *Right.Plan : *Left.Plan;
    ASSERT_EQ(Plan.Cutpoints.size(), 1U);
    const auto &Cut = Plan.Cutpoints.front();
    ASSERT_EQ(Cut.Rank.size(), 1U);
    unsigned Matches = 0;
    for (const auto &I : Cut.Inputs)
      if (I.Side == LowIRLoopSide::Original && I.Temporary == Cut.Rank[0]) {
        ++Matches;
        EXPECT_EQ(I.Location.Space, LowIRLoopSpace::Frame);
        EXPECT_EQ(I.Location.Offset, Side ? 8U : 0U);
        EXPECT_EQ(I.Location.Bytes, Side ? 4U : 8U);
      }
    ASSERT_EQ(Matches, 1U);
  }
  auto Limits = rankPairingLimits();
  Limits.MaxPairingAttempts = 1;
  const auto Before = A.check(B, Limits);
  Limits.MaxPairingAttempts = 2;
  const auto After = A.check(B, Limits);
  EXPECT_EQ(After.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(After.Refinement.Certificate);
  EXPECT_EQ(After.RankPairingAttempts, 1U);
  EXPECT_EQ(After.SolverQueries, Before.SolverQueries);
  EXPECT_EQ(
      After.LastCandidateDiagnostic,
      "loop alignment ranks are not distinct direct inputs of equal width");
}

Program swappedCounterAndTag(bool Swap, bool KeepTag = false) {
  Program P;
  P.Contract.Frame->End = 24;
  P.Contract.ReturnRegisters = {{0, 8}, {8, 8}, {16, 8}};
  const uint64_t Counter = Swap ? 8 : 0, Tag = Swap ? 0 : 8;
  const auto Store = [&](uint64_t Offset, NdVar Value) {
    P.instruction({op(NdOp::INT_ADD, t(16), {r(32), n(Offset)}),
                   op(NdOp::STORE, {}, {t(16), Value})});
  };
  const auto Update = [&](uint64_t Offset, NdOp Code, uint64_t Value) {
    P.instruction({op(NdOp::INT_ADD, t(16), {r(32), n(Offset)}),
                   op(NdOp::LOAD, t(0), {t(16)}),
                   op(Code, t(8), {t(0), n(Value)}),
                   op(NdOp::STORE, {}, {t(16), t(8)})});
  };
  P.block(0, 0x100, {1});
  Store(Counter, r(8));
  Store(Tag, r(16));
  Store(16, n(0));
  P.branch(0x200);
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_ADD, t(16), {r(32), n(Counter)}),
                 op(NdOp::LOAD, t(0), {t(16)}),
                 op(NdOp::INT_EQUAL, t(8, 1), {t(0), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), t(8, 1)})});
  P.block(2, 0x300, {1});
  Update(Counter, NdOp::INT_SUB, 1);
  Update(Tag, NdOp::INT_XOR, 1);
  Update(16, NdOp::INT_ADD, 3);
  P.branch(0x200);
  P.block(3, 0x400, {});
  if (!KeepTag)
    Store(Tag, n(0));
  P.instruction({op(NdOp::INT_ADD, t(16), {r(32), n(16)}),
                 op(NdOp::LOAD, r(0), {t(16)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, DirectRanksExcludeConflictingFrameBindings) {
  const auto A = swappedCounterAndTag(false);
  const auto B = swappedCounterAndTag(true);
  for (const auto *P : {&A, &B}) {
    const auto Self = inferLowIRLoopRefinementPlan(P->Function, A.Contract);
    ASSERT_TRUE(Self.inferred()) << Self.Diagnostic;
    ASSERT_EQ(Self.Plan->Cutpoints.size(), 1U);
    const auto &Cut = Self.Plan->Cutpoints.front();
    ASSERT_EQ(Cut.Rank.size(), 1U);
    std::set<uint64_t> CurrentFrames;
    unsigned Ranks = 0;
    for (const auto &I : Cut.Inputs)
      if (I.Side == LowIRLoopSide::Original &&
          I.Location.Space == LowIRLoopSpace::Frame) {
        CurrentFrames.insert(I.Location.Offset);
        EXPECT_EQ(I.Location.Bytes, 8U);
        if (I.Temporary == Cut.Rank[0]) {
          ++Ranks;
          EXPECT_EQ(I.Location.Offset, P == &A ? 0U : 8U);
        }
      }
    ASSERT_EQ(Ranks, 1U);
    ASSERT_EQ(CurrentFrames, (std::set<uint64_t>{0, 8, 16}));
  }
  EXPECT_FALSE(A.check(B).proved());
  const auto Good = A.check(B, rankPairingLimits());
  ASSERT_TRUE(Good.proved())
      << Good.Diagnostic << ": " << Good.LastCandidateDiagnostic;
  // Rank-only lacks the running-sum equality. Its union must retain that
  // equality while excluding both same-frame mappings across the swapped
  // countdown and independent tag. Final frame bytes remain observed.
  EXPECT_EQ(Good.RankPairingAttempts, 2U);
  const auto Wrong =
      A.check(swappedCounterAndTag(true, true), rankPairingLimits());
  EXPECT_FALSE(Wrong.proved());
  EXPECT_FALSE(Wrong.Refinement.Certificate);
}

Program increasingFrameCounter() {
  Program P;
  P.block(0, 0x100, {1});
  P.instruction({op(NdOp::STORE, {}, {r(32), n(0)})});
  P.branch(0x200);
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::LOAD, t(0), {r(32)}),
                 op(NdOp::INT_LESSEQUAL, t(8, 1), {r(8), t(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), t(8, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::LOAD, t(0), {r(32)}),
                 op(NdOp::INT_ADD, t(8), {t(0), n(1)}),
                 op(NdOp::STORE, {}, {r(32), t(8)})});
  P.branch(0x200);
  P.block(3, 0x400, {});
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, DirectRanksDoNotGuessConstantsOrExpressions) {
  for (bool ConstantPhase : {false, true}) {
    SCOPED_TRACE(ConstantPhase);
    const auto A =
        ConstantPhase ? sequentialCounterLoop(2) : increasingFrameCounter();
    auto B = A;
    B.Function.Blocks.back().Ops.back().Inputs[0] = n(7);
    for (const Program *P : {&A, static_cast<const Program *>(&B)}) {
      const auto Self = inferLowIRLoopRefinementPlan(P->Function, A.Contract);
      ASSERT_TRUE(Self.inferred()) << Self.Diagnostic;
      const auto &Cut = Self.Plan->Cutpoints.front();
      ASSERT_FALSE(Cut.Rank.empty());
      EXPECT_EQ(Cut.Rank.front().isConst(), ConstantPhase);
      EXPECT_TRUE(std::none_of(Cut.Inputs.begin(), Cut.Inputs.end(),
                               [&](const auto &I) {
                                 return I.Side == LowIRLoopSide::Original &&
                                        I.Temporary == Cut.Rank.front();
                               }));
    }
    auto Limits = rankPairingLimits();
    Limits.MaxPairingAttempts = 1;
    const auto Before = A.check(B, Limits);
    Limits.MaxPairingAttempts = 2;
    const auto After = A.check(B, Limits);
    EXPECT_EQ(After.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
    EXPECT_EQ(After.RankPairingAttempts, 1U);
    EXPECT_EQ(After.SolverQueries, Before.SolverQueries);
    EXPECT_EQ(
        After.LastCandidateDiagnostic,
        "loop alignment ranks are not distinct direct inputs of equal width");
    EXPECT_FALSE(After.Refinement.Certificate);
  }
}
} // namespace
