//===- SymMBALazyPolynomialTests.cpp - Bounded polynomial emission --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check deferred flat polynomial construction against independent full-width
/// oracles, completed tie cases, measured work ceilings and resource refusal.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/SmallString.h"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct DenseCarryFixture {
  SymRef Input;
  unsigned Width;
  unsigned HighBits;
  unsigned Degree;
  unsigned Observed;
  bool ComplementParity = false;
  bool MixedMasks = false;
  uint64_t Coefficient = 3;

  unsigned inputWidth(unsigned) const { return Width; }

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    auto Result = llvm::APInt(Width, Coefficient).shl(Width - Observed);
    const auto Mask = ~llvm::APInt::getLowBitsSet(Width, HighBits);
    const auto X = V[0] ^ llvm::APInt(Width, ComplementParity);
    for (unsigned I = 0; I < Degree; ++I) {
      auto High = (V[1] + I) & Mask;
      if (MixedMasks)
        High =
            ((V[1] + I) & ~llvm::APInt(Width, 1)) *
            ((V[2] + 2 * I) & ~llvm::APInt::getLowBitsSet(Width, HighBits - 1));
      Result *= (X + 2 * I) ^ High;
    }
    return Result + V[0] + V[1] + V[2] + V[3];
  }
};

enum class FactorOrder { Natural, Interleaved, Rotated };

DenseCarryFixture denseCarryFixture(SymContext &Ctx, unsigned Width,
                                    unsigned HighBits, unsigned Degree = 48,
                                    unsigned Observed = 7,
                                    bool ComplementParity = false,
                                    bool MixedMasks = false,
                                    uint64_t Coefficient = 3,
                                    FactorOrder Order = FactorOrder::Natural) {
  // Preserve the public expression parser's construction order: the measured
  // cost/work bounds below belong to these two complete public-entry inputs.
  llvm::SmallString<96> Scale;
  llvm::APInt(Width, Coefficient)
      .shl(Width - Observed)
      .toString(Scale, 10, false);
  std::string Expression = "(" + Scale.str().str();
  const std::string X = ComplementParity ? "(x^1)" : "x";
  for (unsigned Position = 0; Position < Degree; ++Position) {
    const unsigned I =
        Order == FactorOrder::Interleaved
            ? 2 * (Position % (Degree / 2)) + Position / (Degree / 2)
        : Order == FactorOrder::Rotated ? (Position + 3) % Degree
                                        : Position;
    std::string High = "((y+" + std::to_string(I) + ")&-" +
                       std::to_string(uint64_t(1) << HighBits) + ")";
    if (MixedMasks)
      High = "(((y+" + std::to_string(I) + ")&-2)*((z+" +
             std::to_string(2 * I) + ")&-" +
             std::to_string(uint64_t(1) << (HighBits - 1)) + "))";
    Expression += "*((" + X + "+" + std::to_string(2 * I) + ")^" + High + ")";
  }
  Expression += ")+(x^y)+2*(x&y)+(z|w)+(z&w)";
  const auto Parsed = parseSymExpr(Ctx, Expression, Width);
  EXPECT_TRUE(Parsed.ok()) << Parsed.Error;
  return {Parsed.Root,      Width,      HighBits,   Degree, Observed,
          ComplementParity, MixedMasks, Coefficient};
}

struct BitPolynomialFixture {
  SymRef Input;
  unsigned Width;
  bool Common;
  bool Complement;

  unsigned inputWidth(unsigned) const { return Width; }

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    bool Bit = Complement;
    if (Common) {
      for (unsigned I = 1; I < 10; ++I) {
        const bool Other = I < 8 ? V[0][I] : V[2][I - 8];
        Bit ^= bool(V[0][0] && V[1][0] && Other);
      }
    } else {
      for (unsigned Word = 0; Word < 3; ++Word)
        for (unsigned I = 0; I < 6; I += 2)
          Bit ^= bool(V[Word][I] && V[Word][I + 1]);
    }
    return llvm::APInt(Width, Bit).shl(Width - 1);
  }
};

struct AdditiveResidueFixture {
  SymRef Input;
  unsigned Width;

  unsigned inputWidth(unsigned) const { return Width; }

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    auto Product = llvm::APInt(Width, 3).shl(Width - 4);
    const auto Mask = ~llvm::APInt(Width, 3);
    for (unsigned I = 0; I < 4; ++I)
      Product *= V[0] + 2 * I + ((V[1] + I) & Mask);
    return Product + V[0] + V[1] + V[2] + V[3];
  }
};

AdditiveResidueFixture additiveResidueFixture(SymContext &Ctx, unsigned Width) {
  llvm::SmallString<96> Scale;
  llvm::APInt(Width, 3).shl(Width - 4).toString(Scale, 10, false);
  std::string Expression = "(" + Scale.str().str();
  for (unsigned I = 0; I < 4; ++I)
    Expression += "*((x+" + std::to_string(2 * I) + ")+((y+" +
                  std::to_string(I) + ")&-4))";
  Expression += ")+(x^y)+2*(x&y)+(z|w)+(z&w)";
  const auto Parsed = parseSymExpr(Ctx, Expression, Width);
  EXPECT_TRUE(Parsed.ok()) << Parsed.Error;
  return {Parsed.Root, Width};
}

BitPolynomialFixture bitPolynomialFixture(SymContext &Ctx, unsigned Width,
                                          bool Common, bool Complement) {
  // Non-oracle variable order and an unused variable detect accidental use of
  // variable ids as x/y/z positions when validating the wide expressions.
  Ctx.mkVar("unused", 1);
  const auto Z = Ctx.mkVar("z", Width), Y = Ctx.mkVar("y", Width);
  const auto X = Ctx.mkVar("x", Width);
  auto K = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  std::vector<SymRef> Terms;
  if (Common) {
    for (unsigned I = 1; I < 10; ++I) {
      const auto Other = I < 8 ? Ctx.mkLShr(X, K(I)) : Ctx.mkLShr(Z, K(I - 8));
      Terms.push_back(Ctx.mkAnd({X, Y, Other}));
    }
  } else {
    for (const auto Word : {X, Y, Z})
      for (unsigned I = 0; I < 6; I += 2)
        Terms.push_back(
            Ctx.mkAnd(Ctx.mkLShr(Word, K(I)), Ctx.mkLShr(Word, K(I + 1))));
  }
  if (Complement)
    Terms.push_back(Ctx.mkConst(llvm::APInt::getAllOnes(Width)));
  const auto Input = Ctx.mkMul(
      Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 1)), Ctx.mkXor(Terms));
  return {Input, Width, Common, Complement};
}

enum class ControlledInput { Division, NarrowExtension, NestedShift };

struct ControlledPolynomialFixture {
  SymRef Input;
  unsigned Width;
  ControlledInput Kind;

  unsigned inputWidth(unsigned Index) const {
    return Width -
           unsigned(Kind == ControlledInput::NarrowExtension && Index == 1);
  }

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    auto A = V[0];
    const auto B = V[1].zextOrTrunc(Width);
    const auto C = V[2];
    if (Kind == ControlledInput::Division)
      A = A.udiv(C | llvm::APInt(Width, 1));
    else if (Kind == ControlledInput::NestedShift)
      A = (A.lshr(4) + B).lshr(4);
    bool Bit = false;
    for (unsigned I = 1; I < 10; ++I) {
      const bool Other = I < 8 ? B[I] : C[I - 8];
      Bit ^= bool(A[0] && B[0] && Other);
    }
    return llvm::APInt(Width, Bit).shl(Width - 1);
  }
};

ControlledPolynomialFixture controlledPolynomialFixture(SymContext &Ctx,
                                                        unsigned Width,
                                                        ControlledInput Kind) {
  Ctx.mkVar("unused", 1);
  const auto Z = Ctx.mkVar("z", Width);
  auto Y = Ctx.mkVar(
      "y", Width - unsigned(Kind == ControlledInput::NarrowExtension));
  const auto X = Ctx.mkVar("x", Width);
  const auto K = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  if (Kind == ControlledInput::NarrowExtension)
    Y = Ctx.mkZExt(Y, Width);
  auto A = X;
  if (Kind == ControlledInput::Division)
    A = Ctx.mkUDiv(X, Ctx.mkOr(Z, K(1)));
  else if (Kind == ControlledInput::NestedShift) {
    // Observed=1 admits both shifts during the source discovery walk. The
    // outer shift later requests five sum bits, so its inner shift is an
    // opaque decision word. Source discovery alone cannot certify plain inputs.
    A = Ctx.mkLShr(Ctx.mkAdd(Ctx.mkLShr(X, K(4)), Y), K(4));
  }
  std::vector<SymRef> Terms;
  for (unsigned I = 1; I < 10; ++I) {
    const auto Other = I < 8 ? Ctx.mkLShr(Y, K(I)) : Ctx.mkLShr(Z, K(I - 8));
    Terms.push_back(Ctx.mkAnd({A, Y, Other}));
  }
  const auto Input = Ctx.mkMul(
      Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 1)), Ctx.mkXor(Terms));
  return {Input, Width, Kind};
}

template <typename Fixture>
void checkFullWidthOracle(SymContext &Ctx, const Fixture &F, SymRef Result) {
  SymEvalPlan Eval(Ctx, Result);
  std::vector<unsigned> Bindings(Ctx.numVars(), 4);
  for (const uint32_t Id : Eval.vars()) {
    const auto &Info = Ctx.varInfo(Id);
    const unsigned Index = Info.Name == "x"   ? 0
                           : Info.Name == "y" ? 1
                           : Info.Name == "z" ? 2
                           : Info.Name == "w" ? 3
                                              : 4;
    ASSERT_LT(Index, 4u) << Info.Name;
    ASSERT_EQ(Info.Width, F.inputWidth(Index)) << Info.Name;
    Bindings[Id] = Index;
  }
  std::mt19937_64 Random(0x4c415a594c4f5742);
  bool Zero = false, Nonzero = false;
  for (unsigned Trial = 0; Trial < 512; ++Trial) {
    std::array<llvm::APInt, 4> Values;
    for (unsigned I = 0; I < Values.size(); ++I) {
      const auto InputWidth = F.inputWidth(I);
      Values[I] = llvm::APInt(InputWidth, 0);
      for (unsigned Bit = 0; Bit < InputWidth; Bit += 64)
        Values[I] |= llvm::APInt(InputWidth, Random()).shl(Bit);
      if (Trial < 256)
        Values[I] = llvm::APInt(InputWidth, Trial * (2 * I + 1));
    }
    if (Trial == 256) {
      // Complemented parity needs a nonzero x with a zero complete result:
      // the product's even branch vanishes, while x+w cancels modulo Width.
      for (unsigned I = 0; I < Values.size(); ++I)
        Values[I] = llvm::APInt(F.inputWidth(I), 0);
      Values[0] = llvm::APInt(F.inputWidth(0), 1);
      Values[3] = llvm::APInt::getAllOnes(F.inputWidth(3));
    }
    const auto Expected = F.oracle(Values);
    const auto Actual = Eval.evalWith([&](uint32_t Id) -> const llvm::APInt * {
      if (Id >= Bindings.size() || Bindings[Id] >= Values.size()) {
        ADD_FAILURE() << "Missing oracle binding for variable id " << Id;
        return nullptr;
      }
      return &Values[Bindings[Id]];
    });
    EXPECT_EQ(Actual, Expected) << Trial;
    Zero |= Expected.isZero();
    Nonzero |= !Expected.isZero();
  }
  EXPECT_TRUE(Zero);
  EXPECT_TRUE(Nonzero);
}

TEST(SymMBALazyPolynomial, DenseCarryEmissionKeepsMeasuredWorkCeilings) {
  // Sharing the parity guard and evaluating odd squares at reduced precision
  // removes repeated bit-level work. Preserve the complete public-entry cost
  // and work gains alongside the independent modular value checks.
  for (const unsigned HighBits : {2u, 3u}) {
    SCOPED_TRACE(HighBits);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, 32, HighBits);
    ASSERT_TRUE(F.Input);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    ASSERT_NE(Result.Expr, F.Input);
    EXPECT_EQ(Result.SizeBefore, 495u);
    EXPECT_EQ(Result.SizeAfter, HighBits == 2 ? 325u : 321u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(Result.Work, HighBits == 2 ? 45000u : 40000u);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, SharedParityGuardsKeepBothBranchesExact) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (bool Complement : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Complement);
      SymContext Ctx;
      const auto F = denseCarryFixture(Ctx, Width, 2, 8, 5, Complement);
      MBAOptions Options;
      Options.MaxWork = 65536;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      ASSERT_NE(Result.Expr, F.Input);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      checkFullWidthOracle(Ctx, F, Result.Expr);
      if (Width <= 64 && solver::Z3Solver::available()) {
        solver::Z3SolverOptions VerifyOptions;
        VerifyOptions.TimeoutMs = 30000;
        solver::Z3Solver Verifier(Ctx, VerifyOptions);
        EXPECT_EQ(Verifier.checkDistinct(F.Input, Result.Expr),
                  solver::SatResult::Unsat)
            << Verifier.reasonUnknown();
      }
    }
}

TEST(SymMBALazyPolynomial, LowWordConstantViewsFinishDegree96MixedProducts) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, Width, 3, 96, 7, false, true);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LT(Result.Work, 60000u);
    EXPECT_LE(Result.SizeAfter, 19u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, PartialBlocksFinishDegree32MixedProducts) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, Width, 2, 32, 7, false, true);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LT(Result.Work, 45000u);
    EXPECT_LE(Result.SizeAfter, 27u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, InterleavedProductsFindTheirExactPairs) {
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned HighBits : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << HighBits);
      SymContext Ctx;
      const auto F = denseCarryFixture(Ctx, Width, HighBits, 128, 8, false,
                                       false, 3, FactorOrder::Interleaved);
      MBAOptions Options;
      Options.MaxWork = 65536;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LE(Result.SizeAfter, 17u);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      checkFullWidthOracle(Ctx, F, Result.Expr);
    }
}

TEST(SymMBALazyPolynomial, PairProbesPreserveMixedProductHeadroom) {
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (const auto [Observed, HighBits] :
         {std::pair{6u, 2u}, std::pair{7u, 2u}, std::pair{7u, 3u}}) {
      SCOPED_TRACE(::testing::Message()
                   << Width << ':' << Observed << ':' << HighBits);
      SymContext Ctx;
      const auto F =
          denseCarryFixture(Ctx, Width, HighBits, 64, Observed, true, true);
      MBAOptions Options;
      Options.MaxWork = 65536;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LE(Result.SizeAfter, 14u);
      checkFullWidthOracle(Ctx, F, Result.Expr);
    }
}

TEST(SymMBALazyPolynomial, UnfinishedPairProbesRetainEveryFactor) {
  for (size_t Work : {512u, 8192u, 32768u}) {
    SCOPED_TRACE(Work);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, 32, 2, 128, 8, false, false, 3,
                                     FactorOrder::Interleaved);
    MBAOptions Options;
    Options.MaxWork = Work;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LE(Result.Work, Work);
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, PartialPairingBoundariesPreserveRefusedWords) {
  for (unsigned Degree : {16u, 17u, 31u, 32u, 33u})
    for (bool Complement : {false, true})
      for (size_t Work : {1024u, 16384u, 65536u}) {
        SCOPED_TRACE(::testing::Message()
                     << Degree << ':' << Complement << ':' << Work);
        SymContext Ctx;
        const auto F =
            denseCarryFixture(Ctx, 32, 2, Degree, 7, Complement, true);
        MBAOptions Options;
        Options.MaxWork = Work;
        const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
        EXPECT_LE(Result.Work, Work);
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        checkFullWidthOracle(Ctx, F, Result.Expr);
      }
}

TEST(SymMBALazyPolynomial, BooleanRecipesShareTheFinalBitProjection) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, Width, 2, 24, 5, false, true);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    // Reconstruct y[1] XOR (y[0] OR z[1]) before the single final mask.
    // Masking every recipe input made this exact reading appear too costly.
    EXPECT_LE(Result.SizeAfter, 25u);
    EXPECT_LT(Result.Work, 14000u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
    if (solver::Z3Solver::available()) {
      solver::Z3SolverOptions VerifyOptions;
      VerifyOptions.TimeoutMs = 30000;
      solver::Z3Solver Verifier(Ctx, VerifyOptions);
      // This fresh verifier serves one formula. Asserting the difference
      // permits Z3's one-shot preprocessing of the complete multiplication.
      ASSERT_TRUE(Verifier.assertDistinct(F.Input, Result.Expr));
      EXPECT_EQ(Verifier.check(), solver::SatResult::Unsat)
          << Verifier.reasonUnknown();
    }
  }
}

TEST(SymMBALazyPolynomial, ScaledComplementsFinishWithoutRepeatedProofSearch) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = additiveResidueFixture(Ctx, Width);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    // Retain the smaller shared projection without re-proving its complement.
    // Deferring the highest-bit coefficient's normalization spent 3346 units.
    EXPECT_LE(Result.SizeAfter, 27u);
    EXPECT_LT(Result.Work, 2200u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
    if (solver::Z3Solver::available()) {
      solver::Z3SolverOptions VerifyOptions;
      VerifyOptions.TimeoutMs = 30000;
      solver::Z3Solver Verifier(Ctx, VerifyOptions);
      EXPECT_EQ(Verifier.checkDistinct(F.Input, Result.Expr),
                solver::SatResult::Unsat)
          << Verifier.reasonUnknown();
    }
  }
}

TEST(SymMBALazyPolynomial, OddCorrectionsRetainWorkForCompleteReconstruction) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, Width, 2, 16, 6, false, true);
    MBAOptions Options;
    Options.MaxWork = 15500;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LE(Result.SizeAfter, 40u);
    EXPECT_LT(Result.Work, Options.MaxWork);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, SignatureOperandsSurviveLowWordInterning) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    // Reduced constant views grow the operand pool while a recursive wide
    // signature still needs its siblings. ASan caught a stale span here.
    // Its mixed carries must also finish within the shared work budget.
    const auto F = denseCarryFixture(Ctx, Width, 2, 47, 7, false, true, 5);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LT(Result.Work, Options.MaxWork);
    EXPECT_LE(Result.SizeAfter, 266u);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, FusedArithmeticFinishesDegree64CarryProducts) {
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned HighBits : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << HighBits);
      SymContext Ctx;
      const auto F = denseCarryFixture(Ctx, Width, HighBits, 64, 8);
      ASSERT_TRUE(F.Input);
      MBAOptions Options;
      Options.MaxWork = 65536;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LE(Result.SizeAfter, 170u);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      checkFullWidthOracle(Ctx, F, Result.Expr);
    }
}

TEST(SymMBALazyPolynomial, BalancedCarryColumnsFinishRotatedProducts) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, Width, 3, 64, 8, false, false, 3,
                                     FactorOrder::Rotated);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LT(Result.Work, 54000u);
    EXPECT_LE(Result.SizeAfter, 170u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, InterruptedCarryColumnsRetainCompleteValues) {
  for (size_t MaxWork : {1u, 1024u, 8192u, 16384u, 32768u, 49151u}) {
    SCOPED_TRACE(MaxWork);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, 64, 3, 64, 8, false, false, 3,
                                     FactorOrder::Rotated);
    MBAOptions Options;
    Options.MaxWork = MaxWork;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LE(Result.Work, MaxWork);
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, DenseFrequenciesFinishWidePartialProducts) {
  for (unsigned Width : {8u, 64u, 65u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, Width, 4, 62, 7, false, true);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    EXPECT_LT(Result.Work, 60000u);
    EXPECT_LE(Result.SizeAfter, 286u);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    checkFullWidthOracle(Ctx, F, Result.Expr);
  }
}

TEST(SymMBALazyPolynomial, ExhaustsDegree64CarryLowWords) {
  for (unsigned HighBits : {2u, 3u}) {
    SCOPED_TRACE(HighBits);
    SymContext Ctx;
    const auto F = denseCarryFixture(Ctx, 8, HighBits, 64, 8);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    ASSERT_LT(Result.Work, Options.MaxWork);
    ASSERT_LE(Result.SizeAfter, 170u);
    SymEvalPlan Eval(Ctx, Result.Expr);
    std::vector<unsigned> Bindings(Ctx.numVars(), 4);
    for (auto Id : Eval.vars()) {
      const auto &Name = Ctx.varInfo(Id).Name;
      const unsigned Index = Name == "x"   ? 0
                             : Name == "y" ? 1
                             : Name == "z" ? 2
                             : Name == "w" ? 3
                                           : 4;
      ASSERT_LT(Index, 4u);
      Bindings[Id] = Index;
    }
    for (unsigned X = 0; X < 256; ++X)
      for (unsigned Y = 0; Y < 256; ++Y) {
        const std::array<llvm::APInt, 4> Values{
            llvm::APInt(8, X), llvm::APInt(8, Y), llvm::APInt(8, X ^ Y),
            llvm::APInt(8, X + Y)};
        const auto Actual =
            Eval.evalWith([&](uint32_t Id) -> const llvm::APInt * {
              if (Id >= Bindings.size() || Bindings[Id] >= Values.size())
                return nullptr;
              return &Values[Bindings[Id]];
            });
        ASSERT_EQ(Actual, F.oracle(Values)) << X << ':' << Y;
      }
  }
}

TEST(SymMBALazyPolynomial, SingleCofactorGroupingFinishesTheSharedBudget) {
  SymContext Ctx;
  const auto F = denseCarryFixture(Ctx, 32, 3, 24, 7);
  MBAOptions Options;
  Options.MaxWork = 65536;
  const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
  // This product's finished reading is larger than its original form. The
  // shared search must finish without wasting work on a one-entry partition.
  EXPECT_LT(Result.Work, Options.MaxWork);
  EXPECT_EQ(Result.SizeBefore, 255u);
  EXPECT_EQ(Result.SizeAfter, 245u);
  checkFullWidthOracle(Ctx, F, Result.Expr);
}

struct CofactorFixture {
  SymRef Input;
  unsigned Width;
  bool Outside;

  unsigned inputWidth(unsigned) const { return Width; }

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    bool High = false;
    for (unsigned I = 0; I < 6; ++I)
      for (unsigned J = I + 1; J < 6; ++J)
        High ^= bool(V[0][I] && V[0][J]);
    bool Low =
        High ^ bool(V[0][0] && V[0][1]) ^ bool(V[0][0] && V[0][1] && V[0][2]);
    if (Outside)
      Low ^= bool(V[0][7] && V[0][0] && V[0][1]) ^
             bool(V[0][7] && V[0][0] && V[0][2]);
    return llvm::APInt(Width, Low ^ bool(V[0][6] && High)).shl(Width - 1);
  }
};

TEST(SymMBALazyPolynomial, CofactorSupportIncludesEveryRemainderVariable) {
  for (unsigned Width : {8u, 32u, 64u, 65u, 128u, 257u})
    for (const bool Outside : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Outside);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width);
      std::array<SymRef, 8> Bits;
      for (unsigned I = 0; I < Bits.size(); ++I)
        Bits[I] = Ctx.mkLShr(X, Ctx.mkConst(Width, I));
      std::vector<SymRef> Terms;
      for (unsigned I = 0; I < 6; ++I)
        for (unsigned J = I + 1; J < 6; ++J) {
          if (I != 0 || J != 1)
            Terms.push_back(Ctx.mkAnd(Bits[I], Bits[J]));
          Terms.push_back(Ctx.mkAnd({Bits[6], Bits[I], Bits[J]}));
        }
      Terms.push_back(Ctx.mkAnd({Bits[0], Bits[1], Bits[2]}));
      if (Outside) {
        Terms.push_back(Ctx.mkAnd({Bits[7], Bits[0], Bits[1]}));
        Terms.push_back(Ctx.mkAnd({Bits[7], Bits[0], Bits[2]}));
      }
      const auto Input = Ctx.mkMul(
          Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 1)), Ctx.mkXor(Terms));
      const CofactorFixture F{Input, Width, Outside};
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      bool Complete = false;
      const auto Result = detail::foldResidueDecisions(
          Ctx, Input, Budget, Storage, false, 65536, nullptr, &Complete);
      ASSERT_TRUE(Complete);
      EXPECT_LE(Ctx.readability(Result), Ctx.readability(Input));
      // The first 256 oracle inputs exhaust all eight bits used by this
      // polynomial; the remaining inputs also exercise native and wide words.
      checkFullWidthOracle(Ctx, F, Result);
    }
}

TEST(SymMBALazyPolynomial, DenseCarryValuesRemainExactAcrossWordBoundaries) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (const unsigned HighBits : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << HighBits);
      SymContext Ctx;
      const auto F = denseCarryFixture(Ctx, Width, HighBits);
      ASSERT_TRUE(F.Input);
      MBAOptions Options;
      Options.MaxWork = 65536;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
      EXPECT_LE(Result.Work, Options.MaxWork);
      // Wide refusal is checked as refusal semantics, not as an optimization.
      // The native performance contract lives in the preceding measured test.
      checkFullWidthOracle(Ctx, F, Result.Expr);
    }
}

TEST(SymMBALazyPolynomial, CompletesCommonFactorsAndFlatTies) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (const bool Common : {false, true})
      for (const bool Complement : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Common << ':' << Complement);
        SymContext Ctx;
        const auto F = bitPolynomialFixture(Ctx, Width, Common, Complement);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        bool Complete = false;
        const auto Result = detail::foldResidueDecisions(
            Ctx, F.Input, Budget, Storage, false, 65536, nullptr, &Complete);
        ASSERT_TRUE(Complete);
        if (Common) {
          ASSERT_NE(Result, F.Input);
          EXPECT_LT(Ctx.readability(Result), Ctx.readability(F.Input));
        } else {
          // Nine disjoint monomials need the flat comparison. Merely returning
          // the source on budget failure does not satisfy the Complete check.
          EXPECT_EQ(Result, F.Input);
        }
        checkFullWidthOracle(Ctx, F, Result);
      }
}

TEST(SymMBALazyPolynomial, ExactAndShortResourcesPreserveCompleteValues) {
  for (unsigned Width : {8u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext FullCtx;
    const auto F = bitPolynomialFixture(FullCtx, Width, true, true);
    detail::WorkBudget FullBudget(65536);
    size_t FullStorage = 1 << 24;
    bool FullComplete = false;
    const auto FullResult =
        detail::foldResidueDecisions(FullCtx, F.Input, FullBudget, FullStorage,
                                     false, 65536, nullptr, &FullComplete);
    ASSERT_TRUE(FullComplete);
    ASSERT_NE(FullResult, F.Input);
    const auto Work = FullBudget.used();
    const size_t Bytes = (1 << 24) - FullStorage;
    ASSERT_GT(Work, 1u);
    ASSERT_GT(Bytes, 1u);
    const auto Expected = FullCtx.toString(FullResult);
    for (const auto [WorkLimit, ByteLimit] : {std::pair{Work, Bytes},
                                              {Work - 1, Bytes},
                                              {Work, Bytes - 1},
                                              {size_t(0), Bytes},
                                              {Work, size_t(0)}}) {
      SCOPED_TRACE(::testing::Message() << WorkLimit << ':' << ByteLimit);
      SymContext Ctx;
      const auto Fixture = bitPolynomialFixture(Ctx, Width, true, true);
      detail::WorkBudget Budget(WorkLimit);
      size_t Storage = ByteLimit;
      bool Complete = false, StorageLimited = false;
      const auto Result = detail::foldResidueDecisions(
          Ctx, Fixture.Input, Budget, Storage, false, WorkLimit,
          &StorageLimited, &Complete);
      EXPECT_LE(Budget.used(), WorkLimit);
      EXPECT_LE(Storage, ByteLimit);
      const bool Exact = WorkLimit == Work && ByteLimit == Bytes;
      EXPECT_EQ(Complete, Exact);
      if (Exact)
        EXPECT_EQ(Ctx.toString(Result), Expected);
      if (WorkLimit == Work && ByteLimit < Bytes)
        EXPECT_TRUE(StorageLimited);
      checkFullWidthOracle(Ctx, Fixture, Result);
    }
  }
}

TEST(SymMBALazyPolynomial, NonPlainDecisionWordsKeepIndependentSemantics) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (const auto Kind :
         {ControlledInput::Division, ControlledInput::NarrowExtension,
          ControlledInput::NestedShift}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << unsigned(Kind));
      SymContext FullCtx;
      const auto F = controlledPolynomialFixture(FullCtx, Width, Kind);
      detail::WorkBudget FullBudget(65536);
      size_t FullStorage = 1 << 24;
      const auto FullResult = detail::foldResidueDecisions(
          FullCtx, F.Input, FullBudget, FullStorage);
      EXPECT_LE(FullCtx.readability(FullResult), FullCtx.readability(F.Input));
      EXPECT_LE(FullBudget.used(), 65536u);
      checkFullWidthOracle(FullCtx, F, FullResult);
      const size_t ShortWork = FullBudget.used() ? FullBudget.used() - 1 : 0;
      for (const auto [Work, Bytes] : {std::pair{size_t(0), size_t(1 << 24)},
                                       {ShortWork, size_t(1 << 24)},
                                       {size_t(65536), size_t(0)}}) {
        SCOPED_TRACE(::testing::Message() << Work << ':' << Bytes);
        SymContext Ctx;
        const auto Fixture = controlledPolynomialFixture(Ctx, Width, Kind);
        detail::WorkBudget Budget(Work);
        size_t Storage = Bytes;
        const auto Result = detail::foldResidueDecisions(
            Ctx, Fixture.Input, Budget, Storage, false, Work);
        EXPECT_LE(Budget.used(), Work);
        EXPECT_LE(Storage, Bytes);
        EXPECT_LE(Ctx.readability(Result), Ctx.readability(Fixture.Input));
        checkFullWidthOracle(Ctx, Fixture, Result);
      }
      // Opaque inputs need not complete. These controls check value retention
      // and refusal bounds without treating a returned source as an
      // improvement.
    }
}

TEST(SymMBALazyPolynomial, NativeCommonFactorsHaveIndependentFullWidthProofs) {
  if (!solver::Z3Solver::available())
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (const bool Complement : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Complement);
      SymContext Ctx;
      const auto F = bitPolynomialFixture(Ctx, Width, true, Complement);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      ASSERT_NE(Result, F.Input);
      solver::Z3SolverOptions Options;
      Options.TimeoutMs = 30000;
      solver::Z3Solver Verifier(Ctx, Options);
      EXPECT_EQ(Verifier.checkDistinct(F.Input, Result),
                solver::SatResult::Unsat)
          << Verifier.reasonUnknown();
    }
}

} // namespace
