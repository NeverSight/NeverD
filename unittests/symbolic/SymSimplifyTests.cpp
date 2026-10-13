//===- SymSimplifyTests.cpp - Symbolic normalization tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymSimplify.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

using namespace neverd::symbolic;

TEST(SymExpressionSimplify, BooleanCarryPairCancelsInsideAnAffineSum) {
  for (unsigned Width : {4U, 8U, 32U, 64U, 128U, 256U}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Value = C.mkAdd({C.mkOr(X, Y), C.mkAnd(X, Y), Z});
    auto Expected = C.mkAdd({X, Y, Z});
    EXPECT_EQ(simplifyExpression(C, Value), Expected);
    auto Different = C.mkAdd({C.mkOr(X, Y), C.mkAnd(X, Z), Z});
    auto Reduced = simplifyExpression(C, Different);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Value, Expected)),
              neverd::solver::SatResult::Unsat);
    EXPECT_EQ(Proof.check(C.mkNe(Different, Reduced)),
              neverd::solver::SatResult::Unsat);
    EXPECT_NE(Reduced, Expected);
  }
}

TEST(SymExpressionSimplify, ExhaustedRewriteWorkKeepsCompletedChildReductions) {
  SymContext C;
  auto High = C.mkVar("unobserved_high", 46);
  auto Flag = C.mkVar("flag", 1);
  auto Low = C.mkVar("unobserved_low", 17);
  auto Packed = C.mkConcat({High, Flag, Low});
  auto Read = C.mkExtract(C.mkLShr(Packed, C.mkConst(64, 17)), 0, 1);
  auto Tail = C.mkVar("tail", 64), Divisor = C.mkVar("divisor", 64);
  for (unsigned I = 0; I < 1000; ++I)
    Tail = C.mkUDiv(Tail, Divisor);
  auto Root = C.mkXor(C.mkZExt(Read, 64), Tail);
  std::set<uint32_t> Seen;
  std::vector<SymRef> Work{Root};
  unsigned Edges = 0;
  while (!Work.empty()) {
    auto Value = Work.back();
    Work.pop_back();
    if (!Seen.insert(Value.index()).second)
      continue;
    Edges += C.numOperands(Value);
    for (auto Child : C.operands(Value))
      Work.push_back(Child);
  }
  ExpressionSimplifyOptions Options;
  Options.Work = Edges + 1 + 100;
  EXPECT_EQ(simplifyExpression(C, Root, Options),
            C.mkXor(C.mkZExt(Flag, 64), Tail));
}

TEST(SymExpressionSimplify, ComplementedAddressTransportCancelsExactly) {
  SymContext C;
  auto Base = C.mkAnd(C.mkVar("base", 64), C.mkConst(64, uint64_t(-16)));
  auto Data = C.mkIte(C.mkVar("tag", 1), C.mkVar("a", 64), C.mkVar("b", 64));
  auto X = C.mkAdd({C.mkConst(64, uint64_t(-160)), Base, Data});
  auto NotX = C.mkSub(C.mkSub(C.mkConst(64, 159), Base), Data);
  auto Hidden = C.mkOr(
      NotX,
      C.mkNot(C.mkOr(X, C.mkNot(C.mkOr(C.mkConst(64, uint64_t(-49)), NotX)))));
  auto Y = C.mkAdd(X, C.mkConst(64, 48));
  auto NotY = C.mkSub(C.mkSub(C.mkConst(64, 111), Base), Data);
  auto P = C.mkNot(C.mkOr(X, C.mkSub(C.mkConst(64, 47), Hidden)));
  auto M = C.mkOr({NotY, C.mkNot(C.mkOr(C.mkConst(64, 48), Y)), P});
  auto Root = C.mkNot(C.mkAdd(P, C.mkXor(P, M)));
  auto Result = simplifyExpression(C, Root);
  EXPECT_EQ(Result, Y);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Result)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymExpressionSimplify, ComplementAndCarryRulesPreserveIndependentInputs) {
  for (unsigned Width : {4U, 8U, 64U, 128U, 256U}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Part = C.mkAnd(X, Y);
    auto Disjoint = C.mkAdd(Part, C.mkXor(Part, Y));
    EXPECT_EQ(simplifyExpression(C, Disjoint), Y);
    auto Carry = C.mkAdd(X, C.mkXor(X, Y));
    auto Reduced = simplifyExpression(C, Carry);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Carry, Reduced)),
              neverd::solver::SatResult::Unsat);
    EXPECT_NE(Reduced, Y);
  }
}

TEST(SymExpressionSimplify, MaskedFlagReadDropsOtherPackedLanes) {
  SymContext C;
  std::vector<SymRef> Lanes;
  for (unsigned Bit = 0; Bit < 16; ++Bit)
    Lanes.push_back(
        C.mkShl(C.mkZExt(C.mkFreshVar(1, "flag_"), 64), C.mkConst(64, Bit)));
  auto Word = C.mkOr(Lanes);
  auto Read = C.mkAnd(C.mkLShr(Word, C.mkConst(64, 6)), C.mkConst(64, 1));
  auto Expected = C.mkZExt(C.mkExtract(Lanes[6], 6, 1), 64);
  EXPECT_EQ(simplifyExpression(C, Read), simplifyExpression(C, Expected));
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Read, simplifyExpression(C, Read))),
            neverd::solver::SatResult::Unsat);
}

TEST(SymExpressionSimplify, SelectedPackedFlagDropsUnobservedInputs) {
  for (unsigned Width : {32U, 64U, 128U, 256U}) {
    SymContext C;
    auto High = C.mkVar("unobserved_high", Width - 18);
    auto Flag = C.mkVar("selected_flag", 1);
    auto Low = C.mkVar("unobserved_low", 17);
    auto Word = C.mkConcat({High, Flag, Low});
    auto Read = C.mkExtract(C.mkLShr(Word, C.mkConst(64, 17)), 0, 1);
    EXPECT_EQ(simplifyExpression(C, Read), Flag) << Width;
  }
}

TEST(SymExpressionSimplify, ExtensionWindowsKeepOnlyTheirOwnedBits) {
  SymContext C;
  auto X = C.mkVar("byte", 8);
  for (bool Signed : {false, true}) {
    auto Extended = Signed ? C.mkSExt(X, 64) : C.mkZExt(X, 64);
    EXPECT_EQ(simplifyExpression(C, C.mkExtract(Extended, 4, 4)),
              C.mkExtract(X, 4, 4));
    EXPECT_EQ(simplifyExpression(C, C.mkExtract(Extended, 16, 8)),
              Signed ? C.mkSExt(C.mkExtract(X, 7, 1), 8) : C.mkZero(8));
    auto Crossing = simplifyExpression(C, C.mkExtract(Extended, 4, 8));
    EXPECT_EQ(Crossing, Signed ? C.mkSExt(C.mkExtract(X, 4, 4), 8)
                               : C.mkZExt(C.mkExtract(X, 4, 4), 8));
  }
}

TEST(SymExpressionSimplify, AlignedDisplacementUsesAnUnconditionalIdentity) {
  SymContext C;
  auto X = C.mkVar("address", 64);
  auto Base = C.mkAnd(X, C.mkConst(64, uint64_t(-16)));
  auto A = C.mkOr(Base, C.mkConst(64, 8));
  auto B = C.mkOr(Base, C.mkConst(64, 3));
  EXPECT_EQ(simplifyExpression(C, C.mkSub(A, B)), C.mkConst(64, 5));
  auto Unknown = C.mkSub(C.mkOr(X, C.mkConst(64, 8)), X);
  auto Reduced = simplifyExpression(C, Unknown);
  EXPECT_FALSE(C.isConst(Reduced));
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Unknown, Reduced)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymExpressionSimplify, ShiftWindowsMatchAnExhaustiveIndependentOracle) {
  for (unsigned SourceWidth = 2; SourceWidth <= 5; ++SourceWidth) {
    SymContext C;
    auto X = C.mkVar("source", SourceWidth);
    for (unsigned Kind = 0; Kind < 3; ++Kind)
      for (unsigned Shift = 0; Shift <= SourceWidth + 1; ++Shift) {
        auto Count = C.mkConst(64, Shift);
        auto Value = Kind == 0   ? C.mkLShr(X, Count)
                     : Kind == 1 ? C.mkAShr(X, Count)
                                 : C.mkShl(X, Count);
        for (unsigned Low = 0; Low < SourceWidth; ++Low)
          for (unsigned Width = 1; Width <= SourceWidth - Low; ++Width) {
            auto Result = simplifyExpression(C, C.mkExtract(Value, Low, Width));
            for (unsigned Input = 0; Input < (1U << SourceWidth); ++Input) {
              llvm::APInt A(SourceWidth, Input);
              auto Expected =
                  Kind == 1 ? A.ashr(std::min(Shift, SourceWidth - 1))
                  : Shift >= SourceWidth ? llvm::APInt(SourceWidth, 0)
                  : Kind == 0            ? A.lshr(Shift)
                                         : A.shl(Shift);
              EXPECT_EQ(C.eval(Result, {A}), Expected.extractBits(Width, Low));
            }
          }
      }
  }
}

TEST(SymExpressionSimplify, SymbolicCountsAndBudgetExhaustionKeepAllInputs) {
  SymContext C;
  auto X = C.mkVar("source", 64), Count = C.mkVar("count", 256);
  auto Root = C.mkExtract(C.mkLShr(X, Count), 3, 8);
  auto Reduced = simplifyExpression(C, Root);
  for (const auto &Shift :
       {llvm::APInt(256, 4), llvm::APInt::getOneBitSet(256, 200)}) {
    auto Input = llvm::APInt::getAllOnes(64);
    EXPECT_EQ(C.eval(Reduced, {Input, Shift}),
              Shift.uge(64)
                  ? llvm::APInt(8, 0)
                  : Input.lshr(Shift.getZExtValue()).extractBits(8, 3));
  }
  ExpressionSimplifyOptions Limits;
  Limits.Nodes = 1;
  const auto Before = C.numNodes();
  EXPECT_EQ(simplifyExpression(C, Root, Limits), Root);
  EXPECT_EQ(C.numNodes(), Before);
  Limits.Nodes = 4096;
  Limits.Work = 0;
  EXPECT_EQ(simplifyExpression(C, Root, Limits), Root);
}

TEST(SymExpressionSimplify, DeepDagTraversalDoesNotUseTheHostStack) {
  SymContext C;
  auto X = C.mkVar("source", 64), Y = C.mkVar("divisor", 64);
  for (unsigned I = 0; I < 5000; ++I)
    X = C.mkUDiv(X, Y);
  auto Root = C.mkExtract(X, 3, 8);
  auto Before = C.numNodes();
  EXPECT_EQ(simplifyExpression(C, Root), Root);
  EXPECT_EQ(C.numNodes(), Before);
}

TEST(SymExpressionSimplify, WideWindowsHaveBitvectorEquivalenceProofs) {
  for (unsigned Width : {8U, 16U, 32U, 64U, 128U, 256U}) {
    SymContext C;
    auto X = C.mkVar("source", Width);
    for (unsigned Shift : {1U, Width / 2, Width - 1, Width, Width + 1})
      for (unsigned Kind = 0; Kind < 3; ++Kind) {
        auto Count = C.mkConst(256, Shift);
        auto Shifted = Kind == 0   ? C.mkLShr(X, Count)
                       : Kind == 1 ? C.mkAShr(X, Count)
                                   : C.mkShl(X, Count);
        auto Root = C.mkExtract(Shifted, Width / 4, Width / 2);
        auto Result = simplifyExpression(C, Root);
        neverd::solver::BitVectorSolver Proof(C);
        EXPECT_EQ(Proof.check(C.mkNe(Root, Result)),
                  neverd::solver::SatResult::Unsat)
            << Width << ":" << Kind << ":" << Shift;
      }
  }
}

TEST(SymExpressionSimplify, NonLowArithmeticWindowsRetainIncomingCarries) {
  SymContext C;
  auto X = C.mkVar("left", 16), Y = C.mkVar("right", 16);
  auto Sum = C.mkExtract(C.mkAdd(X, Y), 8, 8);
  auto Product = C.mkExtract(C.mkMul(X, Y), 8, 8);
  for (auto Root : {Sum, Product}) {
    auto Reduced = simplifyExpression(C, Root);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Root, Reduced)),
              neverd::solver::SatResult::Unsat);
    for (unsigned A : {0U, 1U, 127U, 255U, 256U, 65535U})
      for (unsigned B : {1U, 129U, 255U, 256U, 65535U}) {
        auto Expected = Root == Sum ? A + B : A * B;
        EXPECT_EQ(C.eval(Reduced, {llvm::APInt(16, A), llvm::APInt(16, B)}),
                  llvm::APInt(8, Expected >> 8));
      }
  }
}

TEST(SymExpressionSimplify, InvalidReferencesAreRejected) {
  SymContext C;
  EXPECT_THROW(simplifyExpression(C, SymRef()), std::invalid_argument);
  EXPECT_THROW(simplifyExpression(C, SymRef(uint32_t(C.numNodes() + 1))),
               std::invalid_argument);
}
