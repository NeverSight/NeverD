//===- SymMBAFiniteSelectionTests.cpp - Exact small lookup observations --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate finite table readings over complete input domains and defaults.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace neverd::symbolic;
namespace {
constexpr uint64_t Codes[] = {0, 4, 6, 2, 3, 7, 5, 1};

SymRef table(SymContext &C, SymRef Index, unsigned Width, unsigned Count = 8) {
  auto Value = C.mkConst(Width, 0x80 | Codes[Count - 1]);
  for (unsigned I = Count - 1; I-- > 0;)
    Value = C.mkIte(C.mkEq(Index, C.mkConst(C.width(Index), I)),
                    C.mkConst(Width, 0x80 | Codes[I]), Value);
  return Value;
}

MBAOptions options() {
  MBAOptions O;
  O.MaxAtoms = O.MaxSynthesisAtoms = 5;
  O.MaxOptimalSynthesisAtoms = 2;
  O.MaxWork = 65536;
  return O;
}
} // namespace

TEST(SymMBAFiniteSelection, ExactFieldsKeepEveryUnmatchedInputAtTheDefault) {
  for (unsigned Width : {8u, 64u, 128u})
    for (unsigned Count : {3u, 5u, 8u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Count);
      SymContext C;
      const auto Index = C.mkVar("index", 8);
      const auto Original = table(C, Index, Width, Count);
      const auto Variables = C.numVars();
      const auto R = simplifyMBADeep(C, Original, options());
      if (Count == 8) {
        EXPECT_TRUE(R.Changed);
        EXPECT_LT(C.readability(R.Expr), C.readability(Original));
        EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      }
      EXPECT_EQ(C.numVars(), Variables);
      for (unsigned I = 0; I < 256; ++I)
        EXPECT_EQ(C.eval(R.Expr, {llvm::APInt(8, I)}),
                  llvm::APInt(Width, 0x80 | Codes[std::min(I, Count - 1)]));
      neverd::solver::BitVectorSolver Proof(C);
      EXPECT_EQ(Proof.check(C.mkNe(Original, R.Expr)),
                neverd::solver::SatResult::Unsat);
    }
}

TEST(SymMBAFiniteSelection, RawWideSelectorsDoNotWrapIntoTheSmallDomain) {
  SymContext C;
  const auto Index = C.mkVar("index", 64);
  const auto Original = table(C, Index, 64);
  const auto R = simplifyMBADeep(C, Original, options());
  EXPECT_TRUE(R.Changed);
  for (uint64_t I : std::array<uint64_t, 9>{0, 1, 6, 7, 8, 255, 256,
                                            0x100000001, UINT64_MAX})
    EXPECT_EQ(C.eval(R.Expr, {llvm::APInt(64, I)}).getZExtValue(),
              0x80 | Codes[I < 7 ? I : 7]);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Original, R.Expr)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBAFiniteSelection, MaskedSelectorsNeedNoResidualSelectionChain) {
  SymContext C;
  const auto Index = C.mkVar("index", 8);
  const auto Low = C.mkAnd(Index, C.mkConst(8, 7));
  const auto Original = table(C, Low, 32);
  const auto R = simplifyMBADeep(C, Original, options());
  EXPECT_TRUE(R.Changed);
  std::vector<SymRef> Work{R.Expr};
  while (!Work.empty()) {
    const auto V = Work.back();
    Work.pop_back();
    EXPECT_NE(C.op(V), SymOp::Ite);
    Work.insert(Work.end(), C.operands(V).begin(), C.operands(V).end());
  }
  for (unsigned I = 0; I < 256; ++I)
    EXPECT_EQ(C.eval(R.Expr, {llvm::APInt(8, I)}).getZExtValue(),
              0x80 | Codes[I & 7]);
}

TEST(SymMBAFiniteSelection, UnknownDefaultsAndDuplicateCasesRemainExact) {
  for (bool Duplicate : {false, true}) {
    SymContext C;
    const auto Index = C.mkVar("index", 8), Tail = C.mkVar("tail", 32);
    auto Original =
        C.mkIte(C.mkEq(Index, C.mkConst(8, 1)), C.mkConst(32, 7), Tail);
    Original = C.mkIte(C.mkEq(Index, C.mkConst(8, Duplicate ? 1 : 0)),
                       C.mkConst(32, 11), Original);
    const auto R = simplifyMBADeep(C, Original, options());
    for (unsigned I = 0; I < 256; ++I)
      for (uint64_t T : {0ull, 1ull, 7ull, 11ull, 0xffffffffull}) {
        const auto Expected = I == unsigned(Duplicate) ? 11 : I == 1 ? 7 : T;
        EXPECT_EQ(C.eval(R.Expr, {llvm::APInt(8, I), llvm::APInt(32, T)})
                      .getZExtValue(),
                  Expected);
      }
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Original, R.Expr)),
              neverd::solver::SatResult::Unsat);
  }
}

TEST(SymMBAFiniteSelection, CallerRefusalsDoNotChangeTheTable) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SymContext C;
    const auto Index = C.mkVar("index", 8);
    const auto Original = table(C, Index, 64);
    auto O = options();
    if (Mode == 0)
      O.MaxWork = 0;
    else if (Mode == 1)
      O.MaxTableBytes = 0;
    else
      O.MaxSynthesisAtoms = 0;
    const auto Variables = C.numVars();
    const auto R = simplifyMBADeep(C, Original, O);
    EXPECT_EQ(R.Expr, Original);
    EXPECT_EQ(C.numVars(), Variables);
  }
}

TEST(SymMBAFiniteSelection, CompletePredicateTreesRecoverAShiftedBit) {
  constexpr unsigned Exponents[] = {5, 4, 1, 0, 6, 7, 2, 3};
  for (unsigned Width : {8u, 32u, 64u, 128u})
    for (unsigned InputWidth : {1u, 8u})
      for (bool Fixed : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(InputWidth);
        SCOPED_TRACE(Fixed);
        SymContext C;
        const auto A = C.mkVar("a", InputWidth), B = C.mkVar("b", InputWidth),
                   D = C.mkVar("d", InputWidth);
        const SymRef Inputs[] = {C.mkNe(A, C.mkZero(InputWidth)),
                                 C.mkNe(B, C.mkZero(InputWidth)),
                                 C.mkNe(D, C.mkZero(InputWidth))};
        std::vector<SymRef> Values;
        const auto Mask = Fixed ? llvm::APInt::getHighBitsSet(Width, 1)
                                : llvm::APInt(Width, 0);
        for (unsigned I = 0; I != 8; ++I)
          Values.push_back(C.mkConst(
              Mask |
              llvm::APInt(Width, 1).shl(Exponents[I] % (Width - Fixed))));
        for (unsigned Bit = 0; Bit != 3; ++Bit) {
          std::vector<SymRef> Next;
          for (unsigned I = 0; I != Values.size(); I += 2)
            Next.push_back(C.mkIte(Inputs[Bit], Values[I + 1], Values[I]));
          Values = std::move(Next);
        }
        const auto Original = Values.front();
        const auto R = simplifyMBADeep(C, Original, options());
        if (!Fixed) {
          EXPECT_TRUE(R.Changed);
          EXPECT_EQ(C.op(R.Expr), SymOp::Shl) << C.toString(R.Expr);
        }
        for (unsigned I = 0; I != 8; ++I)
          EXPECT_EQ(
              C.eval(R.Expr, {llvm::APInt(InputWidth, I & 1),
                              llvm::APInt(InputWidth, (I >> 1) & 1),
                              llvm::APInt(InputWidth, (I >> 2) & 1)}),
              Mask | llvm::APInt(Width, 1).shl(Exponents[I] % (Width - Fixed)));
        neverd::solver::BitVectorSolver Proof(C);
        EXPECT_EQ(Proof.check(C.mkNe(Original, R.Expr)),
                  neverd::solver::SatResult::Unsat);
      }
}

TEST(SymMBAFiniteSelection, NestedTablesKeepTheirValueSetForEnclosingProofs) {
  // (p ? 15 : 127) + T is one of 31, 79, 143, 191 or 255 for T over
  // {16, 64, 128}, so its XOR with 3 is never 1. Reading T as shifted bits
  // first hides that value set from the enclosing equality.
  SymContext C;
  const auto Index = C.mkVar("index", 8), P = C.mkVar("p", 1);
  constexpr uint64_t Keys[] = {3, 1, 5, 4, 0, 2},
                     Values[] = {16, 16, 128, 64, 64, 64};
  auto Table = C.mkConst(8, 128);
  for (unsigned I = 6; I-- > 0;)
    Table = C.mkIte(C.mkEq(Index, C.mkConst(8, Keys[I])),
                    C.mkConst(8, Values[I]), Table);
  const auto Sum =
      C.mkAdd(C.mkIte(P, C.mkConst(8, 15), C.mkConst(8, 127)), Table);
  const auto Original = C.mkEq(C.mkConst(8, 1), C.mkXor(C.mkConst(8, 3), Sum));
  for (const auto &Options : {options(), MBAOptions{}}) {
    const auto R = simplifyMBADeep(C, Original, Options);
    EXPECT_EQ(R.Expr, C.mkFalse()) << C.toString(R.Expr);
  }
}

TEST(SymMBAFiniteSelection, RootTableReadingCompetesWithTheMeasuredRoot) {
  // Measuring this root removes its repeated tests and keeps two selections.
  // The literal-field reading of the same tree is longer and must not
  // replace that result.
  SymContext C;
  const auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1);
  const auto Test =
      C.mkEq(C.mkMul(C.mkConst(8, 64), C.mkZExt(Q, 8)), C.mkZExt(P, 8));
  auto Lit = [&](int64_t V) { return C.mkConst(8, uint64_t(V)); };
  const auto Original = C.mkIte(
      Test, C.mkIte(Test, Lit(9), C.mkIte(Test, Lit(33), Lit(-127))),
      C.mkIte(Test, C.mkIte(Test, Lit(3), Lit(1)), C.mkIte(P, Lit(1), Lit(5))));
  const auto Measured = C.mkIte(Test, Lit(9), C.mkIte(P, Lit(1), Lit(5)));
  const auto R = simplifyMBADeep(C, Original, options());
  EXPECT_LE(C.readability(R.Expr), C.readability(Measured))
      << C.toString(R.Expr);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Original, R.Expr)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBAFiniteSelection, RepeatedTestsNeedNoTableReading) {
  // Measuring `p ? (p ? -19 : 2) : 96` drops the repeated test. Reading the
  // tree as fields first would spend most of a small budget on the longer
  // `96 | -115 * zext(p)`.
  SymContext C;
  const auto P = C.mkVar("p", 1);
  auto Lit = [&](int64_t V) { return C.mkConst(8, uint64_t(V)); };
  const auto Original = C.mkIte(P, C.mkIte(P, Lit(-19), Lit(2)), Lit(96));
  for (unsigned Work : {256u, 65536u}) {
    SCOPED_TRACE(Work);
    auto O = options();
    O.MaxWork = Work;
    const auto R = simplifyMBADeep(C, Original, O);
    EXPECT_EQ(R.Expr, C.mkIte(P, Lit(-19), Lit(96))) << C.toString(R.Expr);
    EXPECT_LT(R.Work, 64u);
  }
}

TEST(SymMBAFiniteSelection, OneConditionSelectionsKeepTheirSpelling) {
  // A SIMD lane mask `c ? 0xFF : 0` and a two-case ordinal chain are already
  // their shortest source spellings. Read as fields, the lane mask prints as
  // `(uint8_t)(0 - (uint32_t)c)`.
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  const auto Lane = C.mkIte(C.mkSlt(X, Y), C.mkConst(8, 0xFF), C.mkConst(8, 0));
  const auto Chain = C.mkIte(C.mkEq(X, C.mkConst(8, 0)), C.mkConst(32, 7),
                             C.mkIte(C.mkEq(X, C.mkConst(8, 1)),
                                     C.mkConst(32, 11), C.mkConst(32, 99)));
  for (const auto Original : {Lane, Chain}) {
    const auto R = simplifyMBADeep(C, Original, options());
    EXPECT_EQ(R.Expr, Original) << C.toString(R.Expr);
  }
}

TEST(SymMBAFiniteSelection, FullOrdinalPrefixStillRetainsItsOutsideDefault) {
  SymContext C;
  const auto Index = C.mkVar("index", 8);
  auto Original = C.mkConst(32, 0x99);
  for (unsigned I = 8; I-- > 0;)
    Original = C.mkIte(C.mkEq(Index, C.mkConst(8, I)), C.mkConst(32, Codes[I]),
                       Original);
  const auto R = simplifyMBADeep(C, Original, options());
  for (unsigned I = 0; I != 256; ++I)
    EXPECT_EQ(C.eval(R.Expr, {llvm::APInt(8, I)}).getZExtValue(),
              I < 8 ? Codes[I] : 0x99);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Original, R.Expr)),
            neverd::solver::SatResult::Unsat);
}
