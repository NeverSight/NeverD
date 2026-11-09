//===- SymMBAOutcomeTests.cpp - What the MBA solver reports ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The answers a caller acts on: which outcome a refusal carries, what stands
/// behind a rewrite, and that none of it depends on the word width.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "SymMBATestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <algorithm>

using namespace neverd::symbolic;

namespace {

using test::simplified;
using test::simplifiesTo;
using test::W32;

//===----------------------------------------------------------------------===//
// What the solver reports about what it did
//===----------------------------------------------------------------------===//

TEST(SymMBA, TellsTheReasonsForLeavingAnExpressionAlone) {
  // "Unchanged" covers three different answers, and a caller deciding whether
  // to spend more has to be able to tell them apart.
  SymContext Ctx;
  auto outcomeOf = [&](llvm::StringRef Text, const MBAOptions &Opts = {}) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    EXPECT_TRUE(P.ok()) << Text.str() << ": " << P.Error;
    return simplifyMBA(Ctx, P.Root, Opts).Outcome;
  };

  // Nothing to measure: no input the algebra can drive.
  EXPECT_EQ(outcomeOf("42"), MBAOutcome::NotApplicable);
  // Measured, and nothing shorter exists.
  EXPECT_EQ(outcomeOf("x + y"), MBAOutcome::AlreadyShortest);
  // Refused for width, which is the one refusal a larger budget would undo.
  MBAOptions Tight;
  Tight.MaxAtoms = 2;
  EXPECT_EQ(outcomeOf("x + y + z", Tight), MBAOutcome::TooManyInputs);

  MBAOptions SmallSearch;
  SmallSearch.MaxWork = 1;
  EXPECT_EQ(outcomeOf("x * (y & z) * (y | z) + "
                      "x * (y & ~z) * (~y & z)",
                      SmallSearch),
            MBAOutcome::BudgetExhausted);
}

TEST(SymMBA, SaysWhatStandsBehindARewrite) {
  SymContext Ctx;
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  auto resultOf = [&](llvm::StringRef Text) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    EXPECT_TRUE(P.ok()) << Text.str() << ": " << P.Error;
    return simplifyMBA(Ctx, P.Root, ProofOnly);
  };

  // No samples are run in this test.  Every accepted result therefore has to
  // clear its deterministic coefficient verifier on its own.
  MBAResult Measured = resultOf("(x ^ y) + 2 * (x & y)");
  EXPECT_EQ(Measured.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Measured.Evidence, MBAEvidence::Derivation);
  EXPECT_GT(Measured.Work, 0u);

  // A mask column split is accepted only when every path above each mask is
  // bitwise and therefore cannot carry information across columns.  Sampling
  // is only a defect net for that structural derivation.
  MBAResult Masked = resultOf("((x ^ y) + 2 * (x & y)) & 0xff");
  EXPECT_EQ(Masked.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Masked.Evidence, MBAEvidence::Derivation);

  MBAResult Polynomial = resultOf("(x & y) * (x | y) + (x & ~y) * (~x & y)");
  EXPECT_EQ(Polynomial.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Polynomial.Evidence, MBAEvidence::Derivation);
}

//===----------------------------------------------------------------------===//
// What the solver must refuse to do
//===----------------------------------------------------------------------===//

TEST(SymMBA, LeavesAnExpressionAloneWhenNoFormIsShorter) {
  SymContext Ctx;
  for (const char *Text : {"x + y", "x ^ y", "x & y | z", "x", "42"}) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    ASSERT_TRUE(P.ok());
    MBAResult R = simplifyMBA(Ctx, P.Root);
    EXPECT_FALSE(R.Changed) << Text << " became " << Ctx.toString(R.Expr);
    EXPECT_EQ(R.Expr, P.Root);
  }
}

TEST(SymMBA, StepsAroundWhatTheLinearTheoryDoesNotCover) {
  SymContext Ctx;
  // A quotient, a product of two unknowns, and a variable shift are all
  // outside the algebra; each becomes an input, and what surrounds it is still
  // simplified.
  SymParseResult P =
      parseSymExpr(Ctx, "((x / y) ^ z) + 2 * ((x / y) & z)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(Ctx.toString(R.Expr), "x / y + z");

  SymParseResult Q = parseSymExpr(Ctx, "((x * y) | z) - ((x * y) & z)", W32);
  ASSERT_TRUE(Q.ok());
  EXPECT_EQ(Ctx.toString(simplifyMBA(Ctx, Q.Root).Expr), "z ^ x * y");
}

TEST(SymMBA, TreatsAMaskInsideABitwiseOperatorAsOpaque) {
  // `x & 0xff` is not a bitwise function of x in the sense the measurement
  // needs — it tells bit positions apart — so it has to become an input rather
  // than be measured as one.  Getting this wrong would produce a confidently
  // wrong answer, so the check is that the surrounding algebra still works and
  // the mask survives untouched.
  SymContext Ctx;
  SymParseResult P =
      parseSymExpr(Ctx, "((x & 0xff) ^ y) + 2 * ((x & 0xff) & y)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(Ctx.toString(R.Expr), "(255 & x) + y");
}

TEST(SymMBA, InstantiatesLinearProofsBeforeRestoringScaledInputs) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 256u}) {
    for (unsigned Kind : {0u, 1u, 2u}) {
      for (const llvm::APInt &Coefficient :
           {llvm::APInt(Width, 2), llvm::APInt(Width, 6),
            llvm::APInt::getSignMask(Width)}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Kind);
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width);
        SymRef Y = Ctx.mkVar("y", Width);
        SymRef Source = Kind == 0   ? X
                        : Kind == 1 ? Ctx.mkUDiv(X, Y)
                                    : Ctx.mkAdd(X, Y);
        SymRef Scaled = Ctx.mkMul(Ctx.mkConst(Coefficient), Source);
        SymRef E = Ctx.mkAdd(
            Ctx.mkXor(Source, Scaled),
            Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(Source, Scaled)));
        SymRef Expected = Ctx.mkMul(Ctx.mkConst(Coefficient + 1), Source);
        MBAResult R = simplifyMBA(Ctx, E, ProofOnly);
        if (Kind == 2) {
          SymRef Expanded =
              Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Coefficient + 1), X),
                        Ctx.mkMul(Ctx.mkConst(Coefficient + 1), Y));
          EXPECT_TRUE(R.Expr == Expected || R.Expr == Expanded)
              << Ctx.toString(R.Expr);
        } else {
          EXPECT_EQ(R.Expr, Expected) << Ctx.toString(R.Expr);
        }
        EXPECT_TRUE(R.Changed);
        EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      }
    }
  }
}

TEST(SymMBA, InstantiatesPolynomialProofsBeforeRestoringScaledInputs) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 256u}) {
    for (bool Composite : {false, true}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Composite);
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width);
      SymRef Y = Ctx.mkVar("y", Width);
      SymRef Source = Composite ? Ctx.mkUDiv(X, Y) : X;
      SymRef Scaled = Ctx.mkMul(Ctx.mkConst(Width, 2), Source);
      SymRef E = Ctx.mkAdd(
          Ctx.mkMul(Ctx.mkAnd(Source, Scaled), Ctx.mkOr(Source, Scaled)),
          Ctx.mkMul(Ctx.mkAnd(Source, Ctx.mkNot(Scaled)),
                    Ctx.mkAnd(Ctx.mkNot(Source), Scaled)));
      MBAResult R = simplifyMBA(Ctx, E, ProofOnly);
      EXPECT_EQ(R.Expr, Ctx.mkMul({Ctx.mkConst(Width, 2), Source, Source}))
          << Ctx.toString(R.Expr);
      EXPECT_TRUE(R.Changed);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBA, ProofInstantiationRejectsRelatedButNonEquivalentInputs) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Scaled = Ctx.mkMul(Ctx.mkConst(8, 2), X);
  SymRef And = Ctx.mkAnd(X, Scaled);
  SymRef Cross = Ctx.mkMul(Ctx.mkAnd(X, Ctx.mkNot(Scaled)),
                           Ctx.mkAnd(Ctx.mkNot(X), Scaled));
  SymRef Linear = Ctx.mkAdd(Ctx.mkXor(X, Scaled), And);
  SymRef Polynomial = Ctx.mkAdd(Ctx.mkMul(And, Ctx.mkOr(X, Scaled)),
                                Ctx.mkMul(Ctx.mkConst(8, 2), Cross));
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  MBAResult L = simplifyMBA(Ctx, Linear, ProofOnly);
  MBAResult P = simplifyMBA(Ctx, Polynomial, ProofOnly);
  EXPECT_NE(L.Expr, Ctx.mkMul(Ctx.mkConst(8, 3), X));
  EXPECT_NE(P.Expr, Ctx.mkMul({Ctx.mkConst(8, 2), X, X}));
  std::vector<llvm::APInt> Values(Ctx.numVars(), llvm::APInt(8, 0));
  for (unsigned Value = 0; Value < 256; ++Value) {
    Values[Ctx.varId(X)] = llvm::APInt(8, Value);
    unsigned Twice = (2 * Value) & 255;
    EXPECT_EQ(Ctx.eval(L.Expr, Values).getZExtValue(),
              ((Value ^ Twice) + (Value & Twice)) & 255);
    EXPECT_EQ(Ctx.eval(P.Expr, Values).getZExtValue(),
              ((Value & Twice) * (Value | Twice) +
               2 * (Value & ~Twice) * (~Value & Twice)) &
                  255);
  }
}

TEST(SymMBA, CornerAgreementDoesNotProveMaskedOrNonlinearIdentities) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    const auto Negated = Ctx.mkNeg(X);
    for (const auto Input : {Ctx.mkAnd(X, Ctx.mkOne(Width)), Ctx.mkMul(X, X)}) {
      // Both sides agree at zero and all-ones. At x=2 they differ, so a
      // surviving literal mask or a product must retain its independent
      // abstraction instead of being treated as a uniform linear expression.
      detail::WorkBudget Budget(65536);
      EXPECT_FALSE(detail::proveLinearIdentity(Ctx, Input, Negated, 2, Budget));
      const std::vector<llvm::APInt> Values{llvm::APInt(Width, 2)};
      EXPECT_NE(Ctx.eval(Input, Values), Ctx.eval(Negated, Values));
    }
    const auto Y = Ctx.mkVar("y", Width);
    const auto Complement = Ctx.mkNot(Ctx.mkAdd(X, Y));
    detail::WorkBudget Budget(65536);
    EXPECT_TRUE(detail::proveLinearIdentity(
        Ctx, Complement,
        Ctx.mkSub(Ctx.mkNeg(Ctx.mkAdd(X, Y)), Ctx.mkOne(Width)), 2, Budget));
  }
}

TEST(SymMBA, ProofInstantiationRespectsRefusalAndIndependentInputs) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32);
  SymRef Scaled = Ctx.mkMul(Ctx.mkConst(32, 2), X);
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Scaled),
                       Ctx.mkMul(Ctx.mkConst(32, 2), Ctx.mkAnd(X, Scaled)));
  MBAOptions Options;
  Options.VerifySamples = 0;
  detail::WorkBudget AbstractionBudget(Options.MaxWork);
  auto Abstract = detail::abstractToMBA(Ctx, E, false, &AbstractionBudget,
                                        Options.MaxTableBytes);
  ASSERT_TRUE(Abstract);
  ASSERT_EQ(Abstract->Hidden.size(), 1u);
  SymRef Input(Abstract->Hidden.begin()->first);
  SymRef Form = Ctx.mkAdd(X, Input);
  detail::WorkBudget ProofBudget(Options.MaxWork);
  EXPECT_TRUE(
      detail::proveLinearIdentity(Ctx, Abstract->Body, Form, 2, ProofBudget));
  detail::WorkBudget WrongBudget(Options.MaxWork);
  EXPECT_FALSE(detail::proveLinearIdentity(
      Ctx, Abstract->Body, Ctx.mkMul(Ctx.mkConst(32, 3), X), 2, WrongBudget));
  detail::WorkBudget TinyBudget(3);
  EXPECT_FALSE(
      detail::proveLinearIdentity(Ctx, Abstract->Body, Form, 2, TinyBudget));
  EXPECT_TRUE(TinyBudget.exhausted());
  EXPECT_EQ(TinyBudget.used(), 3u);
  for (size_t Limit : {size_t(0), size_t(1)}) {
    Options.MaxWork = Limit;
    EXPECT_EQ(simplifyMBA(Ctx, E, Options).Expr, E);
  }
  Options.MaxWork = MBAOptions{}.MaxWork;
  Options.MaxTableBytes = 0;
  EXPECT_EQ(simplifyMBA(Ctx, E, Options).Expr, E);
}

TEST(SymMBA, PreservesInvertibleAffineInputsAcrossProofNormalization) {
  // Addition splits into AND + OR for arbitrary words.  Its second operand
  // may itself be affine, and subtracting the candidate must preserve that
  // relationship even when builders fold its coefficient or negation.
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {32u, 64u, 256u}) {
    llvm::APInt HighOdd = llvm::APInt::getOneBitSet(Width, Width - 1) | 3;
    for (const llvm::APInt &Coefficient :
         {llvm::APInt::getAllOnes(Width), llvm::APInt(Width, 3), HighOdd}) {
      for (unsigned Offset : {0u, 7u}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Offset);
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width);
        SymRef Y = Ctx.mkVar("y", Width);
        SymRef Scaled = Ctx.mkMul(Ctx.mkConst(Coefficient), X);
        SymRef Literal = Ctx.mkConst(Width, Offset);
        SymRef Affine = Ctx.mkAdd(Scaled, Literal);
        SymRef Split = Ctx.mkAdd(Ctx.mkAnd(Y, Affine), Ctx.mkOr(Y, Affine));
        SymRef E = Ctx.mkSub(Ctx.mkSub(Split, Y), Literal);
        MBAResult R = simplifyMBA(Ctx, E, ProofOnly);
        EXPECT_EQ(R.Expr, Scaled) << Ctx.toString(R.Expr);
        EXPECT_TRUE(R.Changed);
        EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      }
    }
  }
}

TEST(SymMBA, AffineInputAliasesPreserveNonEquivalentNeighbors) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {32u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef NegX = Ctx.mkNeg(X);
    SymRef And = Ctx.mkAnd(Y, NegX);
    // Replacing OR by another AND destroys the addition decomposition.
    SymRef Neighbor = Ctx.mkSub(Ctx.mkAdd(And, And), Y);
    MBAResult N = simplifyMBA(Ctx, Neighbor, ProofOnly);
    EXPECT_NE(N.Expr, NegX);

    // The original base still has its own meaning underneath bitwise nodes.
    SymRef Mixed = Ctx.mkAdd(Ctx.mkAnd(X, NegX), X);
    MBAResult M = simplifyMBA(Ctx, Mixed, ProofOnly);

    // Multiplication by an even coefficient loses the top bit.  Its base
    // cannot be recovered by an inverse, including at widths above 64 bits.
    SymRef Twice = Ctx.mkMul(Ctx.mkConst(Width, 2), X);
    SymRef Sum = Ctx.mkAdd(Ctx.mkAnd(Y, Twice), Ctx.mkOr(Y, Twice));
    SymRef Even = Ctx.mkSub(Ctx.mkSub(Sum, Y), X);
    MBAResult E = simplifyMBA(Ctx, Even, ProofOnly);

    SymEvalPlan NeighborPlan(Ctx, N.Expr);
    SymEvalPlan MixedPlan(Ctx, M.Expr);
    SymEvalPlan EvenPlan(Ctx, E.Expr);
    std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(Width, 0));
    llvm::APInt High = llvm::APInt::getOneBitSet(Width, Width - 1);
    for (const llvm::APInt &XV :
         {llvm::APInt(Width, 0), llvm::APInt(Width, 1), llvm::APInt(Width, 3),
          High, High + 1, llvm::APInt::getAllOnes(Width)}) {
      for (const llvm::APInt &YV : {llvm::APInt(Width, 0), High + 3}) {
        Assignment[Ctx.varId(X)] = XV;
        Assignment[Ctx.varId(Y)] = YV;
        EXPECT_EQ(NeighborPlan.eval(Assignment), 2 * (YV & -XV) - YV);
        EXPECT_EQ(MixedPlan.eval(Assignment), (XV & -XV) + XV);
        EXPECT_EQ(EvenPlan.eval(Assignment), XV);
      }
    }
  }
}

TEST(SymMBA, RecoversAffineSumsAfterCandidateFlattening) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {32u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef Z = Ctx.mkVar("z", Width);
    auto splitAdd = [&](SymRef A, SymRef B) {
      return Ctx.mkAdd(Ctx.mkXor(A, B),
                       Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(A, B)));
    };
    SymRef P = Ctx.mkAdd(X, Y);
    SymRef Q = Ctx.mkAdd(X, Z);
    SymRef Weighted =
        Ctx.mkAdd({Ctx.mkConst(Width, 7), Ctx.mkMul(Ctx.mkConst(Width, 3), X),
                   Ctx.mkMul(Ctx.mkConst(Width, 5), Y)});
    SymRef Wrapped = Ctx.mkAdd({Ctx.mkNeg(X), Y, Ctx.mkOnes(Width)});
    SymRef Opaque = Ctx.mkUDiv(Y, Z);
    SymRef BitwiseBase = Ctx.mkAnd(X, Y);
    for (const auto &[Before, Expected] :
         {std::pair{splitAdd(X, P), Ctx.mkAdd(X, P)},
          std::pair{splitAdd(X, Weighted), Ctx.mkAdd(X, Weighted)},
          std::pair{splitAdd(X, Wrapped), Ctx.mkAdd(X, Wrapped)},
          std::pair{splitAdd(X, Ctx.mkAdd(X, Opaque)),
                    Ctx.mkAdd({X, X, Opaque})},
          std::pair{splitAdd(BitwiseBase, Ctx.mkAdd(BitwiseBase, Z)),
                    Ctx.mkAdd({BitwiseBase, BitwiseBase, Z})},
          std::pair{Ctx.mkAdd(splitAdd(X, P), splitAdd(X, Q)),
                    Ctx.mkAdd({X, X, P, Q})}}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Ctx.toString(Before));
      MBAResult R = simplifyMBA(Ctx, Before, ProofOnly);
      EXPECT_EQ(R.Expr, Expected) << Ctx.toString(R.Expr);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBA, SharedAffinePivotsAndOpaqueNeighborsRemainExact) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {32u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef Z = Ctx.mkVar("z", Width);
    auto splitAdd = [&](SymRef A, SymRef B) {
      return Ctx.mkAdd(Ctx.mkXor(A, B),
                       Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(A, B)));
    };
    SymRef P = Ctx.mkAdd(X, Ctx.mkOne(Width));
    SymRef Q =
        Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, 3), X), Ctx.mkConst(Width, 7));
    SymRef Shared = Ctx.mkAdd(splitAdd(Y, P), splitAdd(Y, Q));
    SymRef Even =
        Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(Width, 2), X),
                   Ctx.mkMul(Ctx.mkConst(Width, 4), Y), Ctx.mkConst(Width, 6)});
    SymRef EvenSum = splitAdd(X, Even);
    SymRef Opaque = Ctx.mkUDiv(Y, Z);
    SymRef Affine = Ctx.mkAdd(X, Opaque);
    // The nearby OR form omits the carry term; it must not be proved equal
    // to addition merely because both forms contain the same opaque input.
    SymRef Neighbor = Ctx.mkOr(X, Affine);
    SymRef Shifted = Ctx.mkAdd(splitAdd(X, Affine), Ctx.mkOne(Width));
    SymEvalPlan SharedPlan(Ctx, simplifyMBA(Ctx, Shared, ProofOnly).Expr);
    SymEvalPlan EvenPlan(Ctx, simplifyMBA(Ctx, EvenSum, ProofOnly).Expr);
    SymEvalPlan NeighborPlan(Ctx, simplifyMBA(Ctx, Neighbor, ProofOnly).Expr);
    SymEvalPlan ShiftedPlan(Ctx, simplifyMBA(Ctx, Shifted, ProofOnly).Expr);
    std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(Width, 0));
    llvm::APInt High = llvm::APInt::getOneBitSet(Width, Width - 1);
    for (const llvm::APInt &XV :
         {llvm::APInt(Width, 0), llvm::APInt(Width, 1), High, High + 1,
          llvm::APInt::getAllOnes(Width)}) {
      for (const llvm::APInt &YV : {llvm::APInt(Width, 0), High + 3}) {
        Assignment[Ctx.varId(X)] = XV;
        Assignment[Ctx.varId(Y)] = YV;
        Assignment[Ctx.varId(Z)] = llvm::APInt(Width, 3);
        llvm::APInt Quotient = YV.udiv(llvm::APInt(Width, 3));
        EXPECT_EQ(SharedPlan.eval(Assignment), 4 * XV + 2 * YV + 8);
        EXPECT_EQ(EvenPlan.eval(Assignment), 3 * XV + 4 * YV + 6);
        EXPECT_EQ(NeighborPlan.eval(Assignment), XV | (XV + Quotient));
        EXPECT_EQ(ShiftedPlan.eval(Assignment), 2 * XV + Quotient + 1);
      }
    }
  }
}

TEST(SymMBA, NormalizesRestoredAffineCoefficientsBeforeReturning) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {32u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef P =
        Ctx.mkAdd({Ctx.mkConst(Width, 7), Ctx.mkMul(Ctx.mkConst(Width, 3), X),
                   Ctx.mkMul(Ctx.mkConst(Width, 5), Y)});
    SymRef Split = Ctx.mkAdd(Ctx.mkXor(X, P),
                             Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, P)));
    MBAResult R = simplifyMBA(Ctx, Ctx.mkSub(Split, X), ProofOnly);
    EXPECT_EQ(R.Expr, P) << Width << ": " << Ctx.toString(R.Expr);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBA, RetainsAnIndependentIdentityWhenItsAffineRelationIsUnused) {
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  for (unsigned Width : {32u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef P = Ctx.mkAdd(X, Y);
    SymRef Split = Ctx.mkAdd(Ctx.mkXor(X, P),
                             Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, P)));
    // This identity already holds for independent X and P.  Replacing just
    // arithmetic X by P-Y would needlessly introduce an unproved relation
    // with the X that remains visible under the bitwise operators.
    SymRef E = Ctx.mkSub(Ctx.mkSub(Split, X), P);
    detail::WorkBudget Budget(1024);
    EXPECT_TRUE(detail::proveLinearIdentity(Ctx, E, Ctx.mkZero(Width),
                                            ProofOnly.MaxAtoms, Budget));
    MBAResult R = simplifyMBA(Ctx, E, ProofOnly);
    EXPECT_TRUE(Ctx.isConstZero(R.Expr)) << Ctx.toString(R.Expr);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBA, KeepsBitwiseIdentitiesBeforeAffineRebinding) {
  MBAOptions Options;
  Options.VerifySamples = 0;
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 257u})
    for (unsigned Scale : {1u, 3u, 6u})
      for (unsigned Offset : {0u, 1u, 7u})
        for (bool Deep : {false, true}) {
          SCOPED_TRACE(Width);
          SCOPED_TRACE(Scale);
          SCOPED_TRACE(Offset);
          SCOPED_TRACE(Deep);
          SymContext Ctx;
          SymRef X = Ctx.mkVar("x", Width);
          SymRef P = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, Scale), X),
                               Ctx.mkConst(Width, Offset));
          // Both identities hold for independent X and P. Replacing only the
          // arithmetic X by an inverse relation must not hide those readings.
          for (auto [Input, Expected] :
               {std::pair{Ctx.mkAdd(X, Ctx.mkAnd(Ctx.mkNot(X), P)),
                          Ctx.mkOr(X, P)},
                std::pair{Ctx.mkSub(X, Ctx.mkAnd(X, Ctx.mkNot(P))),
                          Ctx.mkAnd(X, P)}}) {
            MBAResult R = Deep ? simplifyMBADeep(Ctx, Input, Options)
                               : simplifyMBA(Ctx, Input, Options);
            EXPECT_LE(Ctx.readability(R.Expr), Ctx.readability(Expected))
                << Ctx.toString(Input) << " -> " << Ctx.toString(R.Expr);
            EXPECT_LE(R.Work, Options.MaxWork);
            if (R.Changed)
              EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
          }
        }
}

TEST(SymMBA, IndependentReadingKeepsRelatedNeighborsExactAtSmallBudgets) {
  for (unsigned Scale : {1u, 3u, 6u})
    for (size_t Work : {size_t(0), size_t(64), MBAOptions{}.MaxWork})
      for (bool Deep : {false, true}) {
        SCOPED_TRACE(Scale);
        SCOPED_TRACE(Work);
        SCOPED_TRACE(Deep);
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", 3);
        SymRef P = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(3, Scale), X), Ctx.mkOne(3));
        MBAOptions Options;
        Options.VerifySamples = 0;
        Options.MaxWork = Work;
        for (SymRef Input : {Ctx.mkAdd(X, Ctx.mkAnd(Ctx.mkNot(X), P)),
                             Ctx.mkAdd(X, Ctx.mkOr(Ctx.mkNot(X), P)),
                             Ctx.mkSub(X, Ctx.mkAnd(X, Ctx.mkNot(P)))}) {
          MBAResult R = Deep ? simplifyMBADeep(Ctx, Input, Options)
                             : simplifyMBA(Ctx, Input, Options);
          EXPECT_LE(R.Work, Work);
          EXPECT_LE(Ctx.readability(R.Expr), Ctx.readability(Input));
          SymEvalPlan Before(Ctx, Input), After(Ctx, R.Expr);
          std::vector<uint64_t> Values(Ctx.numVars(), 0);
          for (unsigned Value = 0; Value != 8; ++Value) {
            Values[Ctx.varId(X)] = Value;
            EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
          }
        }
      }
}

TEST(SymMBA, AffineRecoveryBudgetRefusalIsAtomicAndRetryable) {
  for (unsigned Width : {4u, 64u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef P =
        Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, 3), X), Ctx.mkConst(Width, 7));
    SymRef Q = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, 5), Y), Ctx.mkOne(Width));
    SymRef E = Ctx.mkAdd({X, Y, Ctx.mkAnd(P, Q)});
    detail::WorkBudget NoWork(0);
    auto Original = detail::abstractToMBA(Ctx, E, false, &NoWork);
    ASSERT_TRUE(Original);
    EXPECT_TRUE(NoWork.exhausted());

    detail::WorkBudget Enough(MBAOptions::UnlimitedWork);
    auto Recovered = detail::abstractToMBA(Ctx, E, false, &Enough);
    ASSERT_TRUE(Recovered);
    ASSERT_NE(Recovered->Body, Original->Body);
    ASSERT_GT(Enough.used(), 0u);

    // Include limits late in the operation, after one or both aliases may
    // have been constructed. A refusal returns the complete original body.
    const size_t Step = std::max(size_t(1), Enough.used() / 13);
    for (size_t Limit = 0; Limit < Enough.used(); Limit += Step) {
      SCOPED_TRACE(Limit);
      detail::WorkBudget Tight(Limit);
      auto Declined = detail::abstractToMBA(Ctx, E, false, &Tight);
      ASSERT_TRUE(Declined);
      EXPECT_TRUE(Tight.exhausted());
      EXPECT_LE(Tight.used(), Limit);
      EXPECT_EQ(Declined->Body, Original->Body);
      EXPECT_EQ(Declined->Hidden, Original->Hidden);
    }
    detail::WorkBudget Retry(MBAOptions::UnlimitedWork);
    auto Retried = detail::abstractToMBA(Ctx, E, false, &Retry);
    ASSERT_TRUE(Retried);
    EXPECT_EQ(Retried->Body, Recovered->Body);
    EXPECT_EQ(Retry.used(), Enough.used());

    if (Width == 4) {
      SymRef Restored = Ctx.substitute(Retried->Body, Retried->Hidden);
      std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
      for (uint64_t XV = 0; XV < 16; ++XV)
        for (uint64_t YV = 0; YV < 16; ++YV) {
          Assignment[Ctx.varId(X)] = XV;
          Assignment[Ctx.varId(Y)] = YV;
          EXPECT_EQ(Ctx.evalU64(Restored, Assignment),
                    (XV + YV + (((3 * XV + 7) & 15) & ((5 * YV + 1) & 15))) &
                        15);
        }
    }
  }
}

TEST(SymMBA, AffineRecoveryHonorsBytesWithUnlimitedWork) {
  SymContext Ctx;
  constexpr unsigned Width = 8192;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Y = Ctx.mkVar("y", Width);
  SymRef P =
      Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, 3), X), Ctx.mkConst(Width, 7));
  SymRef E = Ctx.mkAdd(X, Ctx.mkAnd(P, Y));
  detail::WorkBudget NoWork(0);
  auto Original = detail::abstractToMBA(Ctx, E, false, &NoWork);
  ASSERT_TRUE(Original);
  const size_t Nodes = Ctx.numNodes();
  detail::WorkBudget Unlimited(MBAOptions::UnlimitedWork);
  auto Declined = detail::abstractToMBA(Ctx, E, false, &Unlimited, 128);
  ASSERT_TRUE(Declined);
  EXPECT_FALSE(Unlimited.exhausted());
  EXPECT_EQ(Declined->Body, Original->Body);
  EXPECT_EQ(Ctx.numNodes(), Nodes);

  detail::WorkBudget Retry(MBAOptions::UnlimitedWork);
  auto Recovered = detail::abstractToMBA(Ctx, E, false, &Retry);
  ASSERT_TRUE(Recovered);
  EXPECT_NE(Recovered->Body, Original->Body);
}

TEST(SymMBA, AffineRecoverySharesTailWorkAcrossHiddenInputs) {
  // Each hidden input adds a distinct base to the same long affine tail.
  // Expanding that tail independently per input has quadratic work.
  size_t PreviousWork = 0;
  for (unsigned Length : {128u, 256u, 512u}) {
    SCOPED_TRACE(Length);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 64);
    SymRef Tail = X;
    for (unsigned I = 0; I < Length; ++I)
      Tail = Ctx.mkMul(Ctx.mkConst(64, 3), Ctx.mkAdd(Tail, Ctx.mkOne(64)));
    llvm::SmallVector<SymRef, 32> Terms{X};
    for (unsigned I = 0; I < Length / 2; ++I) {
      SymRef Y = Ctx.mkVar("y" + std::to_string(I), 64);
      SymRef Z = Ctx.mkVar("z" + std::to_string(I), 64);
      Terms.push_back(Ctx.mkAnd(Ctx.mkAdd(Tail, Y), Z));
    }
    SymRef E = Ctx.mkAdd(Terms);
    detail::WorkBudget NoWork(0);
    auto Original = detail::abstractToMBA(Ctx, E, false, &NoWork);
    ASSERT_TRUE(Original);
    detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
    auto Recovered = detail::abstractToMBA(Ctx, E, false, &Budget);
    ASSERT_TRUE(Recovered);
    ASSERT_NE(Recovered->Body, Original->Body);
    ASSERT_GT(Budget.used(), 0u);
    if (PreviousWork)
      EXPECT_LT(Budget.used(), 3 * PreviousWork);
    PreviousWork = Budget.used();
  }
}

TEST(SymMBA, DeclinesWhenThereAreMoreInputsThanItWillMeasure) {
  MBAOptions Tight;
  Tight.MaxAtoms = 2;
  SymContext Ctx;
  SymParseResult P = parseSymExpr(Ctx, "(x ^ y ^ z) + 2 * (x & y)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root, Tight);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.NumAtoms, 0u);
}

//===----------------------------------------------------------------------===//
// Widths
//===----------------------------------------------------------------------===//

TEST(SymMBA, WorksAtEveryWidthIncludingOnesNoMachineHas) {
  for (uint32_t Width : {8u, 16u, 32u, 64u, 128u, 256u}) {
    EXPECT_EQ(simplified("(x ^ y) + 2 * (x & y)", Width), "x + y")
        << "at width " << Width;
    EXPECT_EQ(simplified("(x | y) - (x & y)", Width), "x ^ y")
        << "at width " << Width;
  }
}

TEST(SymMBA, AnEvmWordIsMeasuredLikeAnyOther) {
  // Nothing about the measurement is tied to a machine word, so a contract's
  // 256-bit arithmetic is as ordinary here as a byte.
  simplifiesTo("(x ^ y) + 2 * (x & y)", "x + y", 256);
  simplifiesTo("(x | y) + y - (~x & y)", "x + y", 256);
  simplifiesTo("~x + 1", "-x", 256);
}

TEST(SymMBA, ASingleBitWordIsNotASpecialCase) {
  // At one bit, addition is exclusive-or, doubling is zero and negation is the
  // identity, so most of these collapse before the solver is even reached.
  EXPECT_EQ(simplified("(x ^ y) + 2 * (x & y)", 1), "x ^ y");
  EXPECT_EQ(simplified("(x | y) - (x & y)", 1), "x ^ y");
  EXPECT_EQ(simplified("x & ~x", 1), "0");

  // `x + y` is exclusive-or here too, and at the same cost, so which one comes
  // back is a tie broken towards the measured form.
  EXPECT_EQ(simplified("x + y", 1), "x ^ y");
}
} // namespace
