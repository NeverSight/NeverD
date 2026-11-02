//===- SymMBAArithmeticTests.cpp - Modular arithmetic regions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "SymMBATestsDetail.h"

#include <array>
#include <bit>

namespace {
using namespace neverd::symbolic;

TEST(SymMBAArithmetic, LiteralComplementUsesACompleteAffineReading) {
  for (unsigned Width : {1u, 8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    for (SymRef Atom : {X, C.mkUDiv(X, Y)}) {
      for (const llvm::APInt &Constant :
           {llvm::APInt(Width, 1), llvm::APInt::getAllOnes(Width),
            llvm::APInt::getSignedMinValue(Width)}) {
        SymRef Input = C.mkAdd(C.mkConst(Constant), C.mkNot(Atom));
        MBAOptions Opts;
        detail::WorkBudget Budget(64);
        detail::SolveReport Report;
        SymRef Result = detail::solveArithmetic(C, Input, Opts, Budget, Report);
        ASSERT_NE(Result, Input);
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
        EXPECT_EQ(Report.Outcome, MBAOutcome::Rewritten);
        EXPECT_EQ(Report.NumAtoms, 1u);
        EXPECT_FALSE(Report.BudgetExhausted);
        EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(Input));
        for (uint64_t I = 0; I < 256; ++I) {
          auto A = llvm::APInt(Width, I, false, true);
          auto B = llvm::APInt(Width, I * 37, false, true);
          if (I & 1)
            A = ~A;
          if (I & 2)
            B = ~B;
          std::array<llvm::APInt, 2> Values{A, B};
          const llvm::APInt Value = Atom == X ? A
                                    : B.isZero()
                                        ? llvm::APInt::getAllOnes(Width)
                                        : A.udiv(B);
          EXPECT_EQ(C.eval(Input, Values), Constant + ~Value);
          EXPECT_EQ(C.eval(Result, Values), Constant + ~Value);
        }
        for (size_t Work : {size_t(0), size_t(1), Budget.used() - 1}) {
          detail::WorkBudget Short(Work);
          detail::SolveReport Refused;
          EXPECT_EQ(detail::solveArithmetic(C, Input, Opts, Short, Refused),
                    Input);
          EXPECT_TRUE(Refused.BudgetExhausted);
          EXPECT_LE(Short.used(), Work);
        }
        detail::WorkBudget NoBytes(64), KeepComplement(64);
        detail::SolveReport NoBytesReport, KeepReport;
        Opts.MaxTableBytes = 0;
        EXPECT_EQ(
            detail::solveArithmetic(C, Input, Opts, NoBytes, NoBytesReport),
            Input);
        EXPECT_TRUE(NoBytesReport.BudgetExhausted);
        Opts.MaxTableBytes = MBAOptions{}.MaxTableBytes;
        EXPECT_EQ(detail::solveArithmetic(C, Input, Opts, KeepComplement,
                                          KeepReport, false),
                  Input);
      }
    }
  }
}

TEST(SymMBAArithmetic, CancelsDistributedProducts) {
  for (uint32_t Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    test::simplifiesTo("(x + y) * (x - y) - x * x + y * y", "0", Width);
    test::simplifiesTo("(x + y) * z - x * z - y * z", "0", Width);
    test::simplifiesTo("(x + 1) * (x + 1) - x * x - 2 * x", "1", Width);
  }
}

TEST(SymMBAArithmetic, ExtractsSharedFactors) {
  for (uint32_t Width : {8u, 32u, 128u}) {
    SCOPED_TRACE(Width);
    test::simplifiesTo("x * y + x * z", "x * (y + z)", Width);
    test::simplifiesTo("x * x * y + x * x * z", "x * x * (y + z)", Width);
    test::simplifiesTo("x * y + x * z + w", "x * (y + z) + w", Width);
  }
}

TEST(SymMBAArithmetic, SharedFlatProductsKeepProofsIndependentAndBudgeted) {
  for (unsigned Width : {2u, 3u, 8u, 64u, 257u})
    for (bool ExpandComplements : {false, true}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(ExpandComplements);
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
      llvm::SmallVector<SymRef, 32> Tail;
      for (unsigned I = 0; I != 24; ++I)
        Tail.push_back(I % 3 == 0 ? X : I % 3 == 1 ? Y : C.mkNot(Z));
      auto Common = C.mkMul(Tail);
      auto Three = C.mkConst(Width, 3);
      auto Factored =
          C.mkMul({Common, C.mkAdd(X, C.mkMul(Three, Y)), C.mkAdd(Z, W)});
      auto Expanded = C.mkAdd({C.mkMul({Common, X, Z}), C.mkMul({Common, X, W}),
                               C.mkMul({Three, Common, Y, Z}),
                               C.mkMul({Three, Common, Y, W})});
      detail::WorkBudget Full(65536);
      ASSERT_TRUE(detail::proveArithmeticIdentity(C, Expanded, Factored, Full,
                                                  MBAOptions{}.MaxTableBytes,
                                                  ExpandComplements));
      for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
        detail::WorkBudget Budget(Work);
        EXPECT_EQ(detail::proveArithmeticIdentity(C, Expanded, Factored, Budget,
                                                  MBAOptions{}.MaxTableBytes,
                                                  ExpandComplements),
                  Work == Full.used());
        EXPECT_LE(Budget.used(), Work);
      }
      detail::WorkBudget Wrong(65536), NoStorage(65536);
      EXPECT_FALSE(detail::proveArithmeticIdentity(
          C, Expanded, C.mkAdd(Factored, C.mkOne(Width)), Wrong,
          MBAOptions{}.MaxTableBytes, ExpandComplements));
      EXPECT_FALSE(detail::proveArithmeticIdentity(
          C, Expanded, Factored, NoStorage, 0, ExpandComplements));
      for (uint64_t I = 0; I != 256; ++I) {
        std::array<llvm::APInt, 4> Values{
            llvm::APInt(Width, I, false, true),
            ~llvm::APInt(Width, I * 37, false, true),
            llvm::APInt(Width, I * 11, false, true),
            ~llvm::APInt(Width, I * 79, false, true)};
        auto A = Values[0], B = Values[1], D = Values[2], E = Values[3];
        auto Want = (A + llvm::APInt(Width, 3) * B) * (D + E);
        for (unsigned J = 0; J != 8; ++J)
          Want *= A * B * ~D;
        EXPECT_EQ(C.eval(Expanded, Values), Want);
        EXPECT_EQ(C.eval(Factored, Values), Want);
      }
    }
}

TEST(SymMBAArithmetic, ExtractsRepeatedWordCoefficients) {
  for (uint32_t Width : {8u, 32u, 64u, 128u, 257u}) {
    for (int64_t Coefficient : {3, 6, -3, -4}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Coefficient);
      SymContext Ctx;
      const std::string K = std::to_string(Coefficient);
      auto P = parseSymExpr(Ctx, K + "*x+" + K + "*y+(x&y)+5", Width);
      auto Want = parseSymExpr(Ctx, K + "*(x+y)+(x&y)+5", Width);
      ASSERT_TRUE(P.ok());
      ASSERT_TRUE(Want.ok());
      MBAOptions Opts;
      detail::WorkBudget Budget(Opts.MaxWork);
      detail::SolveReport Report;
      SymRef R =
          detail::solveCoefficientFactors(Ctx, P.Root, Opts, Budget, Report);
      EXPECT_EQ(R, Want.Root) << Ctx.toString(R);
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      EXPECT_LT(Ctx.readabilityCost(R), Ctx.readabilityCost(P.Root));
    }
  }
}

TEST(SymMBAArithmetic, CompletesSmallComplementSumsInSelectedAnswer) {
  for (uint32_t Width : {3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto Input =
        parseSymExpr(Ctx, "(x&y)*(-1-12*((x|~y)+(y&~x)))-4*(x|~y)", Width);
    auto Expected = parseSymExpr(Ctx, "11*(x&y)-4*(x|~y)", Width);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Expected.ok());
    MBAOptions Opts;
    detail::WorkBudget Budget(Opts.MaxWork);
    SymRef Answer =
        detail::completeComplementarySums(Ctx, Input.Root, Opts, Budget);
    EXPECT_EQ(Answer, Expected.Root) << Ctx.toString(Answer);
    EXPECT_LT(Ctx.readabilityCost(Answer), Ctx.readabilityCost(Input.Root));
    EXPECT_LE(Budget.used(), Opts.MaxWork);
  }
}

TEST(SymMBAArithmetic, ComplementCompletionKeepsNearMissAndResourceFallback) {
  SymContext Ctx;
  auto Input = parseSymExpr(Ctx, "(x&y)*(-1-12*((x|~y)+(y&~z)))-4*(x|~y)", 8);
  ASSERT_TRUE(Input.ok());
  MBAOptions Opts;
  detail::WorkBudget Full(Opts.MaxWork);
  EXPECT_EQ(detail::completeComplementarySums(Ctx, Input.Root, Opts, Full),
            Input.Root);

  auto Complement =
      parseSymExpr(Ctx, "(x&y)*(-1-12*((x|~y)+(y&~x)))-4*(x|~y)", 8);
  ASSERT_TRUE(Complement.ok());
  detail::WorkBudget Empty(0);
  EXPECT_EQ(
      detail::completeComplementarySums(Ctx, Complement.Root, Opts, Empty),
      Complement.Root);
  Opts.MaxTableBytes = 128;
  detail::WorkBudget NoStorage(Opts.MaxWork);
  EXPECT_EQ(
      detail::completeComplementarySums(Ctx, Complement.Root, Opts, NoStorage),
      Complement.Root);
}

TEST(SymMBAArithmetic, CompletesPartitionedMasksAfterSelection) {
  for (uint32_t Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto Input =
        parseSymExpr(Ctx, "(y & -(y ^ ~x)) + (y & (-2 - (y ^ x)))", Width);
    ASSERT_TRUE(Input.ok());
    MBAOptions Opts;
    detail::WorkBudget Budget(Opts.MaxWork);
    SymRef Answer =
        detail::completeComplementarySums(Ctx, Input.Root, Opts, Budget);
    EXPECT_EQ(Answer, Ctx.mkVar("y", Width)) << Ctx.toString(Answer);
    EXPECT_LE(Budget.used(), Opts.MaxWork);
  }
}

TEST(SymMBAArithmetic, ProvesWideParityPartitionsBeforeCornerMeasurement) {
  for (uint32_t Width : {3u, 8u, 64u, 257u}) {
    for (bool DirectComplement : {false, true}) {
      for (bool WithOffset : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(DirectComplement);
        SCOPED_TRACE(WithOffset);
        SymContext Ctx;
        SymRef Value = Ctx.mkVar("value", Width);
        llvm::SmallVector<SymRef, 32> Vars;
        for (unsigned I = 0; I < 24; ++I)
          Vars.push_back(Ctx.mkVar("x" + std::to_string(I), Width));
        SymRef Mask = Ctx.mkXor(Vars);
        SymRef Opposite;
        if (DirectComplement) {
          Opposite = Ctx.mkNot(Mask);
        } else {
          Vars[0] = Ctx.mkNot(Vars[0]);
          Opposite = Ctx.mkXor(Vars);
        }
        SymRef Input =
            Ctx.mkAdd(Ctx.mkAnd(Value, Mask), Ctx.mkAnd(Value, Opposite));
        SymRef Expected = Value;
        if (WithOffset) {
          Input = Ctx.mkAdd(Input, Ctx.mkConst(Width, 5));
          Expected = Ctx.mkAdd(Value, Ctx.mkConst(Width, 5));
        }
        MBAOptions Opts;
        Opts.MaxWork = 4096;
        Opts.VerifySamples = 0;
        for (bool Deep : {false, true}) {
          MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                                  : simplifyMBA(Ctx, Input, Opts);
          EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
          EXPECT_LE(Result.Work, Opts.MaxWork);
        }
      }
    }
  }
}

TEST(SymMBAArithmetic, ProvesWideOrPartitionsBeforeCornerMeasurement) {
  for (uint32_t Width : {3u, 8u, 64u, 257u}) {
    for (bool DirectComplement : {false, true}) {
      for (bool WithOffset : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(DirectComplement);
        SCOPED_TRACE(WithOffset);
        SymContext Ctx;
        SymRef Value = Ctx.mkVar("value", Width);
        llvm::SmallVector<SymRef, 32> Vars;
        for (unsigned I = 0; I < 24; ++I)
          Vars.push_back(Ctx.mkVar("x" + std::to_string(I), Width));
        SymRef Mask = Ctx.mkXor(Vars);
        SymRef Opposite;
        if (DirectComplement) {
          Opposite = Ctx.mkNot(Mask);
        } else {
          Vars[0] = Ctx.mkNot(Vars[0]);
          Opposite = Ctx.mkXor(Vars);
        }
        SymRef Input =
            Ctx.mkAdd(Ctx.mkOr(Value, Mask), Ctx.mkOr(Value, Opposite));
        SymRef Expected = Ctx.mkAdd(Value, Ctx.mkOnes(Width));
        if (WithOffset) {
          Input = Ctx.mkAdd(Input, Ctx.mkConst(Width, 5));
          Expected = Ctx.mkAdd(Value, Ctx.mkConst(Width, 4));
        }
        MBAOptions Opts;
        Opts.MaxWork = 4096;
        Opts.VerifySamples = 0;
        for (bool Deep : {false, true}) {
          MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                                  : simplifyMBA(Ctx, Input, Opts);
          EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
          EXPECT_LE(Result.Work, Opts.MaxWork);
        }
      }
    }
  }
}

TEST(SymMBAArithmetic, WideOrPartitionsRejectNearMissesAndLowBudgets) {
  constexpr unsigned Width = 8;
  SymContext Ctx;
  SymRef Value = Ctx.mkVar("value", Width);
  llvm::SmallVector<SymRef, 32> Vars;
  for (unsigned I = 0; I < 24; ++I)
    Vars.push_back(Ctx.mkVar("x" + std::to_string(I), Width));
  SymRef Mask = Ctx.mkXor(Vars);
  Vars[0] = Ctx.mkNot(Vars[0]);
  Vars[1] = Ctx.mkNot(Vars[1]);
  SymRef EvenParity = Ctx.mkXor(Vars);
  EXPECT_FALSE(detail::foldPartitionedMaskSum(Ctx, Ctx.mkOr(Value, Mask),
                                              Ctx.mkOr(Value, EvenParity),
                                              llvm::APInt(Width, 0)));

  SymRef OtherValue = Ctx.mkVar("other", Width);
  EXPECT_FALSE(detail::foldPartitionedMaskSum(
      Ctx, Ctx.mkOr(Value, Mask), Ctx.mkOr(OtherValue, Ctx.mkNot(Mask)),
      llvm::APInt(Width, 0)));

  SymRef Input =
      Ctx.mkAdd(Ctx.mkOr(Value, Mask), Ctx.mkOr(Value, Ctx.mkNot(Mask)));
  for (size_t Limit : {size_t(0), size_t(1), size_t(16)}) {
    MBAOptions Opts;
    Opts.MaxWork = Limit;
    Opts.VerifySamples = 0;
    for (bool Deep : {false, true}) {
      MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                              : simplifyMBA(Ctx, Input, Opts);
      EXPECT_EQ(Result.Expr, Input);
      EXPECT_LE(Result.Work, Limit);
    }
  }
  MBAOptions NoStorage;
  NoStorage.MaxTableBytes = 128;
  NoStorage.MaxWork = 4096;
  NoStorage.VerifySamples = 0;
  EXPECT_EQ(simplifyMBA(Ctx, Input, NoStorage).Expr, Input);
  EXPECT_EQ(simplifyMBADeep(Ctx, Input, NoStorage).Expr, Input);
}

TEST(SymMBAArithmetic, ProvesWideFlattenedDeMorganPartitions) {
  for (uint32_t Width : {3u, 64u, 257u}) {
    for (bool OuterOr : {false, true}) {
      for (bool WithOffset : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(OuterOr);
        SCOPED_TRACE(WithOffset);
        SymContext Ctx;
        SymRef Value = Ctx.mkVar("value", Width);
        llvm::SmallVector<SymRef, 32> Vars, Negated;
        for (unsigned I = 0; I < 30; ++I) {
          SymRef Var = Ctx.mkVar("x" + std::to_string(I), Width);
          Vars.push_back(Var);
          Negated.push_back(Ctx.mkNot(Var));
        }
        SymRef Mask = Ctx.mkAnd(Vars);
        SymRef Opposite = Ctx.mkOr(Negated);
        SymRef Input =
            OuterOr
                ? Ctx.mkAdd(Ctx.mkOr(Value, Mask), Ctx.mkOr(Value, Opposite))
                : Ctx.mkAdd(Ctx.mkAnd(Value, Mask), Ctx.mkAnd(Value, Opposite));
        SymRef Expected = OuterOr ? Ctx.mkAdd(Value, Ctx.mkOnes(Width)) : Value;
        if (WithOffset) {
          Input = Ctx.mkAdd(Input, Ctx.mkConst(Width, 5));
          Expected = Ctx.mkAdd(Value, Ctx.mkConst(Width, OuterOr ? 4 : 5));
        }
        MBAOptions Opts;
        Opts.MaxWork = 4096;
        Opts.VerifySamples = 0;
        for (bool Deep : {false, true}) {
          MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                                  : simplifyMBA(Ctx, Input, Opts);
          EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
          EXPECT_LE(Result.Work, Opts.MaxWork);
        }
      }
    }
  }
}

TEST(SymMBAArithmetic, FlattenedDeMorganPartitionsRespectProofAndBounds) {
  constexpr unsigned Width = 64;
  SymContext Ctx;
  SymRef Value = Ctx.mkVar("value", Width);
  SymRef Other = Ctx.mkVar("other", Width);
  llvm::SmallVector<SymRef, 32> Vars, Negated;
  for (unsigned I = 0; I < 30; ++I) {
    SymRef Var = Ctx.mkVar("x" + std::to_string(I), Width);
    Vars.push_back(Var);
    Negated.push_back(Ctx.mkNot(Var));
  }
  SymRef Mask = Ctx.mkAnd(Vars);
  SymRef Opposite = Ctx.mkOr(Negated);
  for (bool OuterOr : {false, true}) {
    SymRef Input = OuterOr ? Ctx.mkAdd(Ctx.mkOr({Value, Other, Mask}),
                                       Ctx.mkOr({Value, Other, Opposite}))
                           : Ctx.mkAdd(Ctx.mkAnd({Value, Other, Mask}),
                                       Ctx.mkAnd({Value, Other, Opposite}));
    SymRef Shared = OuterOr ? Ctx.mkOr(Value, Other) : Ctx.mkAnd(Value, Other);
    SymRef Expected = OuterOr ? Ctx.mkAdd(Shared, Ctx.mkOnes(Width)) : Shared;
    MBAOptions Opts;
    Opts.MaxWork = 4096;
    Opts.VerifySamples = 0;
    for (bool Deep : {false, true}) {
      MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                              : simplifyMBA(Ctx, Input, Opts);
      EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    }

    for (size_t Limit : {size_t(0), size_t(1), size_t(16)}) {
      MBAOptions Bounded = Opts;
      Bounded.MaxWork = Limit;
      for (bool Deep : {false, true}) {
        MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Bounded)
                                : simplifyMBA(Ctx, Input, Bounded);
        EXPECT_EQ(Result.Expr, Input);
        EXPECT_LE(Result.Work, Limit);
      }
    }
    MBAOptions NoStorage = Opts;
    NoStorage.MaxTableBytes = 128;
    EXPECT_EQ(simplifyMBA(Ctx, Input, NoStorage).Expr, Input);
    EXPECT_EQ(simplifyMBADeep(Ctx, Input, NoStorage).Expr, Input);
  }

  Negated[0] = Ctx.mkNot(Ctx.mkVar("replacement", Width));
  SymRef WrongOpposite = Ctx.mkOr(Negated);
  MBAOptions Opts;
  Opts.MaxWork = 4096;
  SymRef WrongAnd =
      Ctx.mkAdd(Ctx.mkAnd(Value, Mask), Ctx.mkAnd(Value, WrongOpposite));
  SymRef WrongOr =
      Ctx.mkAdd(Ctx.mkOr(Value, Mask), Ctx.mkOr(Value, WrongOpposite));
  EXPECT_NE(simplifyMBA(Ctx, WrongAnd, Opts).Expr, Value);
  EXPECT_NE(simplifyMBADeep(Ctx, WrongAnd, Opts).Expr, Value);
  EXPECT_NE(simplifyMBA(Ctx, WrongOr, Opts).Expr,
            Ctx.mkAdd(Value, Ctx.mkOnes(Width)));
  EXPECT_NE(simplifyMBADeep(Ctx, WrongOr, Opts).Expr,
            Ctx.mkAdd(Value, Ctx.mkOnes(Width)));
}

TEST(SymMBAArithmetic, ProvesWideComplementSumsBeforeCornerMeasurement) {
  for (uint32_t Width : {1u, 3u, 64u, 257u}) {
    for (bool XorPair : {false, true}) {
      for (bool WithOffset : {false, true}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(XorPair);
        SCOPED_TRACE(WithOffset);
        SymContext Ctx;
        llvm::SmallVector<SymRef, 32> Vars, Negated;
        for (unsigned I = 0; I < 30; ++I) {
          SymRef Var = Ctx.mkVar("x" + std::to_string(I), Width);
          Vars.push_back(Var);
          Negated.push_back(Ctx.mkNot(Var));
        }
        SymRef A, B;
        if (XorPair) {
          A = Ctx.mkXor(Vars);
          Vars[0] = Negated[0];
          B = Ctx.mkXor(Vars);
        } else {
          A = Ctx.mkAnd(Vars);
          B = Ctx.mkOr(Negated);
        }
        SymRef Input = Ctx.mkAdd(A, B);
        SymRef Expected = Ctx.mkOnes(Width);
        if (WithOffset) {
          Input = Ctx.mkAdd(Input, Ctx.mkConst(Width, 5));
          Expected = Ctx.mkConst(Width, 4);
        }
        MBAOptions Opts;
        Opts.MaxWork = 4096;
        Opts.VerifySamples = 0;
        for (bool Deep : {false, true}) {
          MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                                  : simplifyMBA(Ctx, Input, Opts);
          EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
          EXPECT_LE(Result.Work, Opts.MaxWork);
        }
      }
    }
  }
}

TEST(SymMBAArithmetic, WideComplementSumsRespectBoundsAndNearMisses) {
  constexpr unsigned Width = 8;
  SymContext Ctx;
  llvm::SmallVector<SymRef, 32> Vars, Negated;
  for (unsigned I = 0; I < 30; ++I) {
    SymRef Var = Ctx.mkVar("x" + std::to_string(I), Width);
    Vars.push_back(Var);
    Negated.push_back(Ctx.mkNot(Var));
  }
  SymRef Input = Ctx.mkAdd(Ctx.mkAnd(Vars), Ctx.mkOr(Negated));
  for (size_t Limit : {size_t(0), size_t(1), size_t(16)}) {
    MBAOptions Opts;
    Opts.MaxWork = Limit;
    Opts.VerifySamples = 0;
    for (bool Deep : {false, true}) {
      MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                              : simplifyMBA(Ctx, Input, Opts);
      EXPECT_EQ(Result.Expr, Input);
      EXPECT_LE(Result.Work, Limit);
    }
  }
  MBAOptions NoStorage;
  NoStorage.MaxTableBytes = 128;
  NoStorage.MaxWork = 4096;
  NoStorage.VerifySamples = 0;
  EXPECT_EQ(simplifyMBA(Ctx, Input, NoStorage).Expr, Input);
  EXPECT_EQ(simplifyMBADeep(Ctx, Input, NoStorage).Expr, Input);

  Negated[0] = Ctx.mkNot(Ctx.mkVar("replacement", Width));
  SymRef NearMiss = Ctx.mkAdd(Ctx.mkAnd(Vars), Ctx.mkOr(Negated));
  MBAOptions Opts;
  Opts.MaxWork = 4096;
  EXPECT_NE(simplifyMBA(Ctx, NearMiss, Opts).Expr, Ctx.mkOnes(Width));
  EXPECT_NE(simplifyMBADeep(Ctx, NearMiss, Opts).Expr, Ctx.mkOnes(Width));

  llvm::SmallVector<SymRef, 32> EvenParity(Vars.begin(), Vars.end());
  EvenParity[0] = Ctx.mkNot(Vars[0]);
  EvenParity[1] = Ctx.mkNot(Vars[1]);
  SymRef XorNearMiss = Ctx.mkAdd(Ctx.mkXor(Vars), Ctx.mkXor(EvenParity));
  EXPECT_NE(simplifyMBA(Ctx, XorNearMiss, Opts).Expr, Ctx.mkOnes(Width));
  EXPECT_NE(simplifyMBADeep(Ctx, XorNearMiss, Opts).Expr, Ctx.mkOnes(Width));
}

TEST(SymMBAArithmetic, WideComplementSumsMatchMixedPolarity) {
  constexpr unsigned Width = 64;
  SymContext Ctx;
  llvm::SmallVector<SymRef, 32> AndTerms, OrTerms;
  for (unsigned I = 0; I < 30; ++I) {
    SymRef Var = Ctx.mkVar("x" + std::to_string(I), Width);
    AndTerms.push_back(I % 2 ? Ctx.mkNot(Var) : Var);
    OrTerms.push_back(I % 2 ? Var : Ctx.mkNot(Var));
  }
  SymRef Input = Ctx.mkAdd(Ctx.mkAnd(AndTerms), Ctx.mkOr(OrTerms));
  MBAOptions Opts;
  Opts.MaxWork = 4096;
  Opts.VerifySamples = 0;
  for (bool Deep : {false, true}) {
    MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                            : simplifyMBA(Ctx, Input, Opts);
    EXPECT_EQ(Result.Expr, Ctx.mkOnes(Width));
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAArithmetic, WideParityPartitionRejectsNearMissesAndLowBudgets) {
  SymContext Ctx;
  constexpr unsigned Width = 8;
  SymRef Value = Ctx.mkVar("value", Width);
  llvm::SmallVector<SymRef, 32> Vars;
  for (unsigned I = 0; I < 24; ++I)
    Vars.push_back(Ctx.mkVar("x" + std::to_string(I), Width));
  SymRef Mask = Ctx.mkXor(Vars);
  Vars[0] = Ctx.mkNot(Vars[0]);
  Vars[1] = Ctx.mkNot(Vars[1]);
  SymRef EvenParity = Ctx.mkXor(Vars);
  EXPECT_FALSE(detail::foldPartitionedMaskSum(Ctx, Ctx.mkAnd(Value, Mask),
                                              Ctx.mkAnd(Value, EvenParity),
                                              llvm::APInt(Width, 0)));

  Vars[23] = Ctx.mkVar("replacement", Width);
  EXPECT_FALSE(detail::foldPartitionedMaskSum(Ctx, Ctx.mkAnd(Value, Mask),
                                              Ctx.mkAnd(Value, Ctx.mkXor(Vars)),
                                              llvm::APInt(Width, 0)));

  SymRef DifferentValue = Ctx.mkVar("other", Width);
  EXPECT_FALSE(detail::foldPartitionedMaskSum(
      Ctx, Ctx.mkAnd(Value, Mask), Ctx.mkAnd(DifferentValue, Ctx.mkNot(Mask)),
      llvm::APInt(Width, 0)));

  SymRef Input =
      Ctx.mkAdd(Ctx.mkAnd(Value, Mask), Ctx.mkAnd(Value, Ctx.mkNot(Mask)));
  for (size_t Limit : {size_t(0), size_t(1), size_t(16)}) {
    MBAOptions Opts;
    Opts.MaxWork = Limit;
    Opts.VerifySamples = 0;
    for (bool Deep : {false, true}) {
      MBAResult Result = Deep ? simplifyMBADeep(Ctx, Input, Opts)
                              : simplifyMBA(Ctx, Input, Opts);
      EXPECT_EQ(Result.Expr, Input);
      EXPECT_LE(Result.Work, Limit);
    }
  }
  MBAOptions NoStorage;
  NoStorage.MaxTableBytes = 128;
  NoStorage.MaxWork = 4096;
  NoStorage.VerifySamples = 0;
  EXPECT_EQ(simplifyMBA(Ctx, Input, NoStorage).Expr, Input);
  EXPECT_EQ(simplifyMBADeep(Ctx, Input, NoStorage).Expr, Input);
}

TEST(SymMBAArithmetic, WideParityMatchingUsesEveryOperandExactlyOnce) {
  for (unsigned Width : {3u, 64u, 257u}) {
    SymContext Ctx;
    SymRef Value = Ctx.mkVar("value", Width);
    llvm::SmallVector<SymRef, 24> Vars;
    for (unsigned I = 0; I < 24; ++I)
      Vars.push_back(Ctx.mkVar("x" + std::to_string(I), Width));
    for (uint32_t Trial = 0; Trial < 64; ++Trial) {
      const uint32_t A = (Trial * 0x9e3779b9u) & 0xffffffu;
      const uint32_t B = (Trial * 0x7f4a7c15u + 0x1b873593u) & 0xffffffu;
      llvm::SmallVector<SymRef, 24> ATerms, BTerms;
      for (unsigned I = 0; I < Vars.size(); ++I) {
        ATerms.push_back(A & (1u << I) ? Ctx.mkNot(Vars[I]) : Vars[I]);
        BTerms.push_back(B & (1u << I) ? Ctx.mkNot(Vars[I]) : Vars[I]);
      }
      SymRef Result = detail::foldPartitionedMaskSum(
          Ctx, Ctx.mkAnd(Value, Ctx.mkXor(ATerms)),
          Ctx.mkAnd(Value, Ctx.mkXor(BTerms)), llvm::APInt(Width, 0));
      if (std::popcount(A ^ B) & 1u)
        EXPECT_EQ(Result, Value) << "width " << Width << ", trial " << Trial;
      else
        EXPECT_FALSE(Result.isValid())
            << "width " << Width << ", trial " << Trial;
    }
  }
}

TEST(SymMBAArithmetic, PartitionCompletionRequiresSharedFactorAndMasks) {
  SymContext Ctx;
  MBAOptions Opts;
  for (const char *Text : {
           "(y & -(y ^ ~x)) + (z & (-2 - (y ^ x)))",
           "(y & -(y ^ ~x)) + (y & (-3 - (y ^ x)))",
       }) {
    auto Input = parseSymExpr(Ctx, Text, 8);
    ASSERT_TRUE(Input.ok());
    detail::WorkBudget Budget(Opts.MaxWork);
    EXPECT_EQ(detail::completeComplementarySums(Ctx, Input.Root, Opts, Budget),
              Input.Root);
  }

  auto Shared = parseSymExpr(
      Ctx, "((x | z) & -(y ^ ~x)) + ((x | z) & (-2 - (y ^ x)))", 8);
  ASSERT_TRUE(Shared.ok());
  detail::WorkBudget Budget(Opts.MaxWork);
  SymRef Answer =
      detail::completeComplementarySums(Ctx, Shared.Root, Opts, Budget);
  EXPECT_EQ(Answer, Ctx.mkOr(Ctx.mkVar("x", 8), Ctx.mkVar("z", 8)))
      << Ctx.toString(Answer);
}

TEST(SymMBAArithmetic, XorComplementsRequireOddOperandParity) {
  for (uint32_t Width : {1u, 8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    MBAOptions Opts;
    auto Complement = parseSymExpr(Ctx, "(x ^ ~y ^ z) + (x ^ y ^ z)", Width);
    ASSERT_TRUE(Complement.ok());
    detail::WorkBudget Budget(Opts.MaxWork);
    EXPECT_EQ(
        detail::completeComplementarySums(Ctx, Complement.Root, Opts, Budget),
        Ctx.mkOnes(Width));

    auto Even = parseSymExpr(Ctx, "(x ^ ~y ^ ~z) + (x ^ y ^ z)", Width);
    ASSERT_TRUE(Even.ok());
    detail::WorkBudget EvenBudget(Opts.MaxWork);
    EXPECT_EQ(
        detail::completeComplementarySums(Ctx, Even.Root, Opts, EvenBudget),
        Even.Root);
  }
}

TEST(SymMBAArithmetic, RechecksRegionExposedByMaskCompletion) {
  constexpr const char *InputText =
      "3*((a & -(a ^ ~b)) + (a & (-2 - (a ^ b)))) - 2*(a ^ b) "
      "- 4*(a & b) + ((c | d) + (c & d))";
  for (uint32_t Width : {8u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto Input = parseSymExpr(Ctx, InputText, Width);
    auto Expected = parseSymExpr(Ctx, "a - 2*b + c + d", Width);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Expected.ok());
    MBAResult Answer = simplifyMBADeep(Ctx, Input.Root);
    EXPECT_EQ(Answer.Expr, Expected.Root) << Ctx.toString(Answer.Expr);
    EXPECT_EQ(Answer.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAArithmetic, CoefficientGroupsPreserveOpaqueAtomsAndRemainders) {
  test::simplifiesTo("6*(x/y)+6*(x>>y)+3*(x&y)", "6*((x/y)+(x>>y))+3*(x&y)",
                     32);
  test::simplifiesTo("5*x+5*y+7*z+7*w", "5*(x+y)+7*(z+w)", 128);
  test::simplifiesTo("6*x+6*y+6", "6*(x+y+1)", 257);
}

TEST(SymMBAArithmetic, CoefficientSearchHonorsWorkAndStorageLimits) {
  for (bool LimitStorage : {false, true}) {
    SymContext Ctx;
    auto P = parseSymExpr(Ctx, "6*x+6*y+3*z", 257);
    ASSERT_TRUE(P.ok());
    MBAOptions Opts;
    if (LimitStorage)
      Opts.MaxTableBytes = 128;
    else
      Opts.MaxWork = 10;
    detail::WorkBudget Budget(Opts.MaxWork);
    detail::SolveReport Report;
    EXPECT_EQ(
        detail::solveCoefficientFactors(Ctx, P.Root, Opts, Budget, Report),
        P.Root);
    EXPECT_TRUE(Report.BudgetExhausted);
    EXPECT_LE(Budget.used(), Opts.MaxWork);
  }
}

TEST(SymMBAArithmetic, InterruptedCoefficientConstructionKeepsItsInput) {
  SymContext Ctx;
  auto P = parseSymExpr(Ctx, "6*x+6*y+6", 257);
  auto Want = parseSymExpr(Ctx, "6*(x+y+1)", 257);
  ASSERT_TRUE(P.ok());
  ASSERT_TRUE(Want.ok());
  MBAOptions Opts;
  detail::WorkBudget Full(Opts.MaxWork);
  detail::SolveReport Complete;
  ASSERT_EQ(detail::solveCoefficientFactors(Ctx, P.Root, Opts, Full, Complete),
            Want.Root);
  for (size_t Limit = 0; Limit <= Full.used(); ++Limit) {
    detail::WorkBudget Budget(Limit);
    detail::SolveReport Report;
    SymRef R =
        detail::solveCoefficientFactors(Ctx, P.Root, Opts, Budget, Report);
    EXPECT_TRUE(R == P.Root || R == Want.Root);
    EXPECT_LE(Budget.used(), Limit);
    if (R != P.Root)
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    else
      EXPECT_TRUE(Report.BudgetExhausted);
  }
}

TEST(SymMBAArithmetic, PreservesOpaqueOperationsAsExactAtoms) {
  test::simplifiesTo("(x / z) * (y + 1) - (x / z) * y", "x / z", 32);
  test::simplifiesTo("(x & z) * (y + 1) - (x & z) * y", "x & z", 32);
  test::simplifiesTo("(x >> z) * (y + 1) - (x >> z) * y", "x >> z", 32);
}

TEST(SymMBAArithmetic, CoefficientsWrapAtTheDeclaredWidth) {
  test::simplifiesTo("128 * (x + y) * z + 128 * x * z + 128 * y * z", "0", 8);
  test::simplifiesTo("255 * (x + y) * z + x * z + y * z", "0", 8);
}

TEST(SymMBAArithmetic, ShallowAndDeepKeepTheOriginalPolynomialOpportunity) {
  for (bool Deep : {false, true}) {
    for (const char *Text : {"(x+1)*(x+1)-x*x-2*x", "(x+y)*(x-y)-x*x+y*y+1"}) {
      SymContext Ctx;
      auto P = parseSymExpr(Ctx, Text, 32);
      ASSERT_TRUE(P.ok());
      MBAResult R =
          Deep ? simplifyMBADeep(Ctx, P.Root) : simplifyMBA(Ctx, P.Root);
      EXPECT_EQ(R.Expr, Ctx.mkOne(32)) << Text << ": " << Ctx.toString(R.Expr);
      EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAArithmetic, SkipsOrdinaryLinearRegionsWithoutSpendingWork) {
  SymContext Ctx;
  auto P = parseSymExpr(Ctx, "(x ^ y) + 2 * (x & y)", 32);
  ASSERT_TRUE(P.ok());
  MBAOptions Opts;
  detail::WorkBudget Budget(0);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report), P.Root);
  EXPECT_EQ(Budget.used(), 0u);
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAArithmetic, WorkAndStorageLimitsKeepTheOriginalExpression) {
  for (bool LimitStorage : {false, true}) {
    SymContext Ctx;
    auto P = parseSymExpr(Ctx, "(x+y)*(x-y)-x*x+y*y", 128);
    ASSERT_TRUE(P.ok());
    MBAOptions Opts;
    if (LimitStorage)
      Opts.MaxTableBytes = 128;
    else
      Opts.MaxWork = 10;
    detail::WorkBudget Budget(Opts.MaxWork);
    detail::SolveReport Report;
    const size_t Nodes = Ctx.numNodes();
    EXPECT_EQ(detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report),
              P.Root);
    EXPECT_TRUE(Report.BudgetExhausted);
    EXPECT_EQ(Report.Outcome, MBAOutcome::BudgetExhausted);
    EXPECT_EQ(Ctx.numNodes(), Nodes);
    EXPECT_LE(Budget.used(), Opts.MaxWork);
  }
}

TEST(SymMBAArithmetic, SharedExpansionStopsBeforeAllocatingAnUnboundedTable) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32);
  SymRef Y = Ctx.mkVar("y", 32);
  SymRef E = Ctx.mkAdd(X, Y);
  for (unsigned I = 0; I != 16; ++I)
    E = Ctx.mkAdd(Ctx.mkMul(E, E), Ctx.mkOne(32));
  MBAOptions Opts;
  Opts.MaxWork = 5000;
  detail::WorkBudget Budget(Opts.MaxWork);
  detail::SolveReport Report;
  const size_t Nodes = Ctx.numNodes();
  EXPECT_EQ(detail::solveArithmetic(Ctx, E, Opts, Budget, Report), E);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_EQ(Ctx.numNodes(), Nodes);
  EXPECT_LE(Budget.used(), Opts.MaxWork);
}

TEST(SymMBAArithmetic, NormalizesDeepArithmeticWithoutRecursiveTraversal) {
  SymContext Ctx;
  SymRef E = Ctx.mkVar("x", 32);
  SymRef One = Ctx.mkOne(32);
  SymRef Two = Ctx.mkConst(llvm::APInt(32, 2));
  for (unsigned I = 0; I != 10000; ++I)
    E = Ctx.mkMul(Two, Ctx.mkAdd(E, One));
  MBAOptions Opts;
  detail::WorkBudget Budget(Opts.MaxWork);
  detail::SolveReport Report;
  SymRef R = detail::solveArithmetic(Ctx, E, Opts, Budget, Report);
  EXPECT_EQ(R, Ctx.mkConst(-llvm::APInt(32, 2)));
  EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAArithmetic, DoesNotExpandAnAlreadyFactoredProduct) {
  SymContext Ctx;
  auto P = parseSymExpr(Ctx, "(x+y)*(z+w)", 32);
  ASSERT_TRUE(P.ok());
  MBAOptions Opts;
  Opts.AllowGrowth = true;
  detail::WorkBudget Budget(Opts.MaxWork);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report), P.Root);
}

TEST(SymMBAArithmetic, DerivedFormsAgreeAtEverySmallWidthAssignment) {
  for (const char *Text :
       {"(x+y)*(x-y)-x*x+y*y", "(x+y)*z-x*z-y*z", "x*x*y+x*x*z+x*z",
        "7*(x+y)*z+x*z+3*y*z", "(x/y)*(z+1)-(x/y)*z", "6*x+6*y+3*z",
        "-3*x-3*y+(x&z)", "5*(x/y)+5*(x>>y)+3*z"}) {
    SymContext Ctx;
    auto P = parseSymExpr(Ctx, Text, 4);
    ASSERT_TRUE(P.ok());
    MBAOptions Opts;
    detail::WorkBudget Budget(Opts.MaxWork);
    detail::SolveReport Report;
    SymRef R = detail::solveArithmetic(Ctx, P.Root, Opts, Budget, Report);
    EXPECT_LE(Ctx.readabilityCost(R), Ctx.readabilityCost(P.Root));
    llvm::SmallVector<uint64_t, 3> Values(Ctx.numVars(), 0);
    const uint64_t Assignments = uint64_t(1) << (4 * Ctx.numVars());
    for (uint64_t I = 0; I != Assignments; ++I) {
      for (size_t J = 0; J != Values.size(); ++J)
        Values[J] = (I >> (4 * J)) & 15;
      ASSERT_EQ(Ctx.evalU64(P.Root, Values), Ctx.evalU64(R, Values))
          << Text << " at " << I;
    }
  }
}
} // namespace

namespace {
TEST(SymMBAArithmetic, SharesExactAffineInputsWithoutRequiringAnInverse) {
  const std::pair<const char *, const char *> Cases[] = {
      {"(x | 2*y) - 2*y", "x & ~(2*y)"},
      {"(x | (6*y + 7)) - 6*y - 7", "x & ~(6*y + 7)"},
      {"x + (-y | ~(x | y - 1))", "x - y"},
      {"x - y | ~(y - x - 1 | y - 1)", "x - y"},
      {"x | y | ((x | 2*y) - 2*y)", "x | y"},
      {"(x + y) ^ (y + x)", "0"},
      {"(6*x + 7) ^ ~(7 + 6*x)", "-1"}};
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  for (unsigned Width : {4u, 8u, 16u, 32u, 64u, 257u}) {
    for (const auto &[Text, Expected] : Cases) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Text);
      SymContext Ctx;
      auto P = parseSymExpr(Ctx, Text, Width);
      auto Q = parseSymExpr(Ctx, Expected, Width);
      ASSERT_TRUE(P.ok());
      ASSERT_TRUE(Q.ok());
      auto R = simplifyMBADeep(Ctx, P.Root, Opts);
      EXPECT_EQ(R.Expr, Q.Root) << Ctx.toString(R.Expr);
      if (R.Changed)
        EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
      if (Width != 4)
        continue;
      std::vector<uint64_t> Values(Ctx.numVars(), 0);
      for (unsigned X = 0; X < 16; ++X)
        for (unsigned Y = 0; Y < 16; ++Y) {
          auto XV = Ctx.findVar("x");
          auto YV = Ctx.findVar("y");
          if (XV.has_value())
            Values[*XV] = X;
          if (YV.has_value())
            Values[*YV] = Y;
          EXPECT_EQ(Ctx.evalU64(R.Expr, Values), Ctx.evalU64(P.Root, Values));
        }
    }
  }
}

TEST(SymMBAArithmetic, AffineAliasesKeepDistinctOffsetsAndNoninvertibleInputs) {
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  for (const char *Text :
       {"2*x | 2*x + 1", "2*x ^ x", "-x | ~(y | x)", "(3*x + 7) ^ (3*x + 8)",
        "x | ((x | 2*y) - y)", "x - y | ~(y - x - 2 | y - 1)"}) {
    SymContext Ctx;
    auto P = parseSymExpr(Ctx, Text, 4);
    ASSERT_TRUE(P.ok());
    auto R = simplifyMBADeep(Ctx, P.Root, Opts);
    std::vector<uint64_t> Values(Ctx.numVars(), 0);
    for (unsigned X = 0; X < 16; ++X)
      for (unsigned Y = 0; Y < 16; ++Y) {
        auto XV = Ctx.findVar("x");
        auto YV = Ctx.findVar("y");
        if (XV.has_value())
          Values[*XV] = X;
        if (YV.has_value())
          Values[*YV] = Y;
        ASSERT_EQ(Ctx.evalU64(R.Expr, Values), Ctx.evalU64(P.Root, Values))
            << Text << " at " << X << ", " << Y;
      }
  }
}

TEST(SymMBAArithmetic, AbsorbsCompoundBooleanOperandsDuringConstruction) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  SymRef Y = Ctx.mkVar("y", 64);
  SymRef Z = Ctx.mkVar("z", 64);
  SymRef P = Ctx.mkMul(Ctx.mkConst(64, 2), X);
  EXPECT_EQ(Ctx.mkAnd(P, Ctx.mkOr(Y, P)), P);
  EXPECT_EQ(Ctx.mkOr(P, Ctx.mkAnd(Y, P)), P);
  EXPECT_EQ(Ctx.mkAnd({P, Z, Ctx.mkOr(Y, P)}), Ctx.mkAnd(P, Z));
  EXPECT_EQ(Ctx.mkOr({P, Z, Ctx.mkAnd(Y, P)}), Ctx.mkOr(P, Z));
  EXPECT_NE(Ctx.mkAnd(P, Ctx.mkOr(Y, X)), P);
  EXPECT_NE(Ctx.mkOr(P, Ctx.mkAnd(Y, X)), P);
}

TEST(SymMBAArithmetic, RevisitedRegionsShareTheOriginalWorkBudget) {
  SymContext Ctx;
  auto P =
      parseSymExpr(Ctx, "-(x | y) - (x & y) + ((x | 2*x) & ~(-1 - 2*x))", 64);
  ASSERT_TRUE(P.ok());
  auto Expected = parseSymExpr(Ctx, "x - y", 64);
  ASSERT_TRUE(Expected.ok());
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  auto Complete = simplifyMBADeep(Ctx, P.Root, Opts);
  EXPECT_EQ(Complete.Expr, Expected.Root) << Ctx.toString(Complete.Expr);
  auto Again = simplifyMBADeep(Ctx, Complete.Expr, Opts);
  EXPECT_EQ(Again.Expr, Complete.Expr);
  ASSERT_GT(Complete.Work, 0u);
  for (size_t Limit : {size_t(0), size_t(1), Complete.Work / 2}) {
    Opts.MaxWork = Limit;
    auto Limited = simplifyMBADeep(Ctx, P.Root, Opts);
    EXPECT_LE(Limited.Work, Limit);
    if (!Limited.Changed)
      EXPECT_EQ(Limited.Outcome, MBAOutcome::BudgetExhausted);
  }
}
TEST(SymMBAArithmetic, RecognizesComplementarySelfNegatingCoefficients) {
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  for (unsigned Width : {4u, 8u, 64u, 257u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef High = Ctx.mkConst(llvm::APInt::getOneBitSet(Width, Width - 1));
    SymRef Scaled = Ctx.mkMul(High, X);
    llvm::APInt Offset(Width, 3);
    SymRef A = Ctx.mkAdd(Scaled, Ctx.mkConst(Offset));
    SymRef B = Ctx.mkAdd(Scaled, Ctx.mkConst(~Offset));
    auto R = simplifyMBADeep(Ctx, Ctx.mkXor(A, B), Opts);
    EXPECT_EQ(R.Expr, Ctx.mkOnes(Width)) << Ctx.toString(R.Expr);
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
  }
}

} // namespace

namespace {

TEST(SymMBAArithmetic,
     LongDistributionProofRetainsCoefficientsAndMultiplicities) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
    llvm::SmallVector<SymRef, 64> Factors;
    for (unsigned I = 0; I != 48; ++I)
      Factors.push_back(I % 3 == 0 ? X : I % 3 == 1 ? C.mkNot(Y) : Z);
    auto Tail = C.mkMul(Factors), Scale = C.mkConst(Width, 6);
    auto Sum = C.mkAdd(X, Y);
    auto Factored = C.mkMul({Scale, Tail, Sum, C.mkAdd(Y, Z), C.mkAdd(Z, W)});
    llvm::SmallVector<SymRef, 8> Terms;
    for (auto A : {X, Y})
      for (auto B : {Y, Z})
        for (auto D : {Z, W})
          Terms.push_back(C.mkMul({Scale, Tail, A, B, D}));
    auto Before = C.mkAdd(Terms);
    // Identical unrelated summands may be cancelled, including opaque words.
    auto Extra = C.mkXor(C.mkAdd(X, W), Y);
    Before = C.mkAdd(Before, Extra);
    auto After = C.mkAdd(Factored, Extra);
    detail::WorkBudget Full(65536);
    ASSERT_TRUE(detail::proveArithmeticIdentity(C, Before, After, Full,
                                                MBAOptions{}.MaxTableBytes,
                                                /*ExpandComplements=*/false));
    EXPECT_LT(Full.used(), 8192u);
    for (size_t Work : {size_t(0), size_t(1), Full.used() - 1, Full.used()}) {
      detail::WorkBudget Budget(Work);
      EXPECT_EQ(detail::proveArithmeticIdentity(C, Before, After, Budget,
                                                MBAOptions{}.MaxTableBytes,
                                                /*ExpandComplements=*/false),
                Work == Full.used());
      EXPECT_LE(Budget.used(), Work);
    }
    detail::WorkBudget Wrong(65536), NoStorage(65536);
    EXPECT_FALSE(detail::proveArithmeticIdentity(
        C, Before, C.mkAdd(After, C.mkMul(Tail, Scale)), Wrong,
        MBAOptions{}.MaxTableBytes, /*ExpandComplements=*/false));
    EXPECT_FALSE(detail::proveArithmeticIdentity(C, Before, After, NoStorage, 0,
                                                 /*ExpandComplements=*/false));
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 4> Values{
          llvm::APInt(Width, I, false, true),
          ~llvm::APInt(Width, I * 37, false, true),
          llvm::APInt(Width, I * 11, false, true),
          ~llvm::APInt(Width, I * 79, false, true)};
      auto A = Values[0], B = Values[1], D = Values[2], E = Values[3];
      auto Want = llvm::APInt(Width, 6) * (A + B) * (B + D) * (D + E);
      for (unsigned J = 0; J != 16; ++J)
        Want *= A * ~B * D;
      Want += (A + E) ^ B;
      EXPECT_EQ(C.eval(Before, Values), Want);
      EXPECT_EQ(C.eval(After, Values), Want);
    }
  }
}

} // namespace
