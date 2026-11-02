//===- SymMBAComplementArithmeticTests.cpp - Word complements ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymParse.h"

#include <array>
#include <random>

using namespace neverd::symbolic;

namespace {

TEST(SymMBAComplementArithmetic, KeepsRepeatedComplementCoordinatesCompact) {
  for (unsigned Width : {3u, 8u, 64u, 65u, 257u}) {
    SymContext C;
    SymRef X = C.mkVar("x", Width), Y = C.mkVar("y", Width),
           Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 32> Factors(24, C.mkNot(X));
    SymRef Power = C.mkMul(Factors);
    SymRef Input = C.mkAdd(C.mkMul(Power, Y), C.mkMul(Power, Z));
    MBAOptions Options;
    Options.MaxWork = 4096;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    SymRef Result = detail::solveArithmetic(C, Input, Options, Budget, Report);
    EXPECT_EQ(Result, C.mkMul(Power, C.mkAdd(Y, Z)));
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    EXPECT_FALSE(Budget.exhausted());
    for (uint64_t Seed = 0; Seed != 128; ++Seed) {
      llvm::APInt A(Width, Seed), B(Width, Seed * 37), D(Width, Seed * 53);
      if (Seed & 1)
        A.flipAllBits();
      if (Seed & 2)
        B.flipAllBits();
      llvm::APInt Expected(Width, 1);
      for (unsigned I = 0; I != 24; ++I)
        Expected *= ~A;
      Expected *= B + D;
      EXPECT_EQ(C.eval(Result, {A, B, D}), Expected);
    }
  }
}

TEST(SymMBAComplementArithmetic, ReexpandsNewUsesInTheChosenCoordinate) {
  for (unsigned Width : {3u, 8u, 64u, 65u, 257u}) {
    SymContext C;
    SymRef X = C.mkVar("x", Width);
    llvm::SmallVector<SymRef, 8> Factors(8, C.mkNot(X));
    SymRef Before = C.mkMul(Factors);
    // Independently written coefficients of (1+x)^8 = (~x)^8.
    const unsigned Coefficients[] = {1, 8, 28, 56, 70, 56, 28, 8, 1};
    llvm::SmallVector<SymRef, 9> Terms;
    for (unsigned I = 0; I != 9; ++I) {
      llvm::SmallVector<SymRef, 9> Term(I, X);
      Term.push_back(C.mkConst(Width, Coefficients[I]));
      Terms.push_back(C.mkMul(Term));
    }
    SymRef After = C.mkAdd(Terms);
    detail::WorkBudget Full(65536);
    ASSERT_TRUE(detail::proveArithmeticIdentity(C, Before, After, Full,
                                                size_t(1) << 24));
    const size_t Required = Full.used();
    ASSERT_GT(Required, 0u);
    detail::WorkBudget Exact(Required), Short(Required - 1), Wrong(65536),
        NoStorage(65536);
    EXPECT_TRUE(detail::proveArithmeticIdentity(C, Before, After, Exact,
                                                size_t(1) << 24));
    EXPECT_FALSE(detail::proveArithmeticIdentity(C, Before, After, Short,
                                                 size_t(1) << 24));
    EXPECT_FALSE(detail::proveArithmeticIdentity(
        C, Before, C.mkAdd(After, C.mkOne(Width)), Wrong, size_t(1) << 24));
    EXPECT_FALSE(
        detail::proveArithmeticIdentity(C, Before, After, NoStorage, 0));
    for (unsigned I = 0; I != 128; ++I) {
      llvm::APInt Value(Width, I);
      if (I & 1)
        Value.flipAllBits();
      llvm::APInt Expected(Width, 1);
      for (unsigned J = 0; J != 8; ++J)
        Expected *= ~Value;
      EXPECT_EQ(C.eval(Before, {Value}), Expected);
      EXPECT_EQ(C.eval(After, {Value}), Expected);
    }
  }
}

TEST(SymMBAComplementArithmetic, CancelsAcrossSumsAndProducts) {
  const std::pair<const char *, const char *> Cases[] = {
      {"-1-~x+(y^z)", "x+(y^z)"}, {"3*~x+3*x+(y^z)", "-3+(y^z)"},
      {"x*y+x*~y", "-x"},         {"x*y+x*~(y+1)", "-2*x"},
      {"(x/z)*y+~(x/z)*y", "-y"}, {"~(x+y)+x+y+z*w", "z*w-1"}};
  for (unsigned W : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    for (const auto &[InputText, ExpectedText] : Cases) {
      SCOPED_TRACE(W);
      SCOPED_TRACE(InputText);
      SymContext C;
      auto Input = parseSymExpr(C, InputText, W);
      auto Expected = parseSymExpr(C, ExpectedText, W);
      ASSERT_TRUE(Input.ok());
      ASSERT_TRUE(Expected.ok());
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(Options.MaxWork);
      detail::SolveReport Report;
      SymRef Result =
          detail::solveArithmetic(C, Input.Root, Options, Budget, Report);
      EXPECT_EQ(Result, Expected.Root) << C.toString(Result);
      if (Result != Input.Root)
        EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAComplementArithmetic, LeavesBitwiseConsumersOpaque) {
  for (unsigned W : {3u, 64u, 257u}) {
    SymContext C;
    auto Input = parseSymExpr(C, "(x&~y)*(z+1)-(x&~y)*z", W);
    auto Expected = parseSymExpr(C, "x&~y", W);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Expected.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Expected.Root);
  }
}

TEST(SymMBAComplementArithmetic, PreservesNearbyNonidentitiesExhaustively) {
  for (const char *Text : {"-2-~x+(y^z)", "x*y+x*~(y+1)", "x*~y+~x*y",
                           "~(x*y)+x*y+z", "x*~(y+z)+x*y+x*z", "x&~(y+z)"}) {
    SymContext C;
    auto Input = parseSymExpr(C, Text, 3);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    SymRef Result =
        detail::solveArithmetic(C, Input.Root, Options, Budget, Report);
    llvm::SmallVector<uint64_t, 3> Values(C.numVars(), 0);
    const uint64_t Count = uint64_t(1) << (3 * C.numVars());
    for (uint64_t I = 0; I < Count; ++I) {
      for (size_t J = 0; J < Values.size(); ++J)
        Values[J] = (I >> (3 * J)) & 7;
      ASSERT_EQ(C.evalU64(Result, Values), C.evalU64(Input.Root, Values))
          << Text << " at " << I;
    }
    EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(Input.Root));
  }
}

TEST(SymMBAComplementArithmetic, RefusesIncompleteWorkAndWideStorage) {
  for (size_t Limit : {size_t(0), size_t(1), size_t(16), size_t(64)}) {
    SymContext C;
    auto Input = parseSymExpr(C, "x*y+x*~y", 128);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = Limit;
    detail::WorkBudget Budget(Limit);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Input.Root);
    EXPECT_TRUE(Report.BudgetExhausted);
    EXPECT_LE(Budget.used(), Limit);
  }
  SymContext C;
  auto Input = parseSymExpr(C, "x*y+x*~y", 8192);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.MaxTableBytes = 128;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  const size_t Nodes = C.numNodes();
  EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
            Input.Root);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_EQ(C.numNodes(), Nodes);
}

TEST(SymMBAComplementArithmetic, WalksDeepComplementChainsIteratively) {
  SymContext C;
  SymRef X = C.mkVar("x", 64), One = C.mkOne(64), E = X;
  for (unsigned I = 0; I != 2048; ++I)
    E = C.mkNot(C.mkAdd(E, One));
  E = C.mkAdd(C.mkAdd(E, C.mkNot(X)), One);
  MBAOptions Options;
  Options.VerifySamples = 0;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  EXPECT_EQ(detail::solveArithmetic(C, E, Options, Budget, Report),
            C.mkZero(64));
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAComplementArithmetic, ReusesSharedComplementPolynomials) {
  for (unsigned Count : {8u, 64u}) {
    SymContext C;
    SymRef T = C.mkXor(C.mkVar("x", 128), C.mkVar("y", 128));
    SymRef A = C.mkAdd(T, C.mkOne(128)), B = C.mkNot(T);
    llvm::SmallVector<SymRef, 32> Terms;
    for (unsigned I = 0; I != Count; ++I) {
      SymRef V = C.mkVar("v" + std::to_string(I), 128);
      Terms.push_back(C.mkMul(V, A));
      Terms.push_back(C.mkMul(V, B));
    }
    SymRef Input = C.mkAdd(Terms);
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = 100000;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input, Options, Budget, Report),
              C.mkZero(128));
    EXPECT_FALSE(Report.BudgetExhausted);
    EXPECT_LE(Budget.used(), Options.MaxWork);
  }
}

TEST(SymMBAComplementArithmetic, DefersCompoundOnlyComplementsToLinearRegions) {
  for (const char *Text : {"z+~(x+y)", "z+3*~(x+y)", "~(x+y)+x+y"}) {
    SymContext C;
    auto Input = parseSymExpr(C, Text, 64);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    const size_t Nodes = C.numNodes();
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Input.Root);
    EXPECT_EQ(Budget.used(), 0u);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_FALSE(Report.BudgetExhausted);
  }
  SymContext C;
  auto Input = parseSymExpr(C, "~(x+y)+x+y", 64);
  ASSERT_TRUE(Input.ok());
  MBAOptions Options;
  Options.VerifySamples = 0;
  EXPECT_EQ(simplifyMBADeep(C, Input.Root, Options).Expr, C.mkOnes(64));
}

TEST(SymMBAComplementArithmetic, KeepsLayeredLinearComplementWorkBounded) {
  for (unsigned Depth : {8u, 32u, 64u}) {
    SymContext C;
    SymRef Variables[] = {C.mkVar("x", 64), C.mkVar("y", 64), C.mkVar("z", 64),
                          C.mkVar("w", 64)};
    SymRef Input = Variables[0];
    for (unsigned I = 0; I != Depth; ++I)
      Input = C.mkNot(C.mkAdd(Variables[I % 4], Input));
    MBAOptions Options;
    Options.VerifySamples = 0;
    MBAResult Result = simplifyMBADeep(C, Input, Options);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(Result.Work, 400u * (Depth + 1)) << Depth;
    EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
    for (const std::array<uint64_t, 4> Values :
         {std::array<uint64_t, 4>{0, 0, 0, 0},
          {1, 2, 3, 4},
          {UINT64_MAX, UINT64_MAX - 1, 1, 0},
          {uint64_t(1) << 63, 1, uint64_t(1) << 62, 7}})
      EXPECT_EQ(C.evalU64(Result.Expr, Values), C.evalU64(Input, Values));
  }
}

TEST(SymMBAComplementArithmetic, ReusesOnlyCompatibleCompletedBases) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width),
         Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 16> Powers(12, C.mkNot(X));
    const auto Power = C.mkMul(Powers);
    const auto Input = C.mkAdd(C.mkMul({C.mkConst(Width, 6), Power, Y}),
                               C.mkMul({C.mkConst(Width, 10), Power, Z}));
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(65536);
    Budget.MemoOwner = &C;
    Budget.MemoCapacity = 1024;
    for (bool Expand : {false, true}) {
      detail::SolveReport Report;
      const auto R =
          detail::solveArithmetic(C, Input, Options, Budget, Report, Expand);
      EXPECT_LT(C.readabilityCost(R), C.readabilityCost(Input));
      EXPECT_FALSE(Report.BudgetExhausted);
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      std::mt19937_64 Random(0x434F4D50);
      for (unsigned I = 0; I != 128; ++I) {
        llvm::SmallVector<llvm::APInt, 3> Values;
        for (unsigned J = 0; J != 3; ++J) {
          llvm::APInt V(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            V |= llvm::APInt(Width, Random(), false, true).shl(Bit);
          Values.push_back(V);
        }
        auto P = llvm::APInt(Width, 1);
        for (unsigned K = 0; K != 12; ++K)
          P *= ~Values[0];
        const auto Want = P * (6 * Values[1] + 10 * Values[2]);
        EXPECT_EQ(C.eval(R, Values), Want);
      }
    }
    // Introducing the uncomplemented word changes coordinate selection.
    // A warmed opaque basis must not hide the full-width complement identity.
    const auto Near = C.mkAdd(Input, C.mkMul(Power, X));
    detail::SolveReport Report;
    const auto R = detail::solveArithmetic(C, Near, Options, Budget, Report);
    for (uint64_t V : std::array<uint64_t, 5>{0, 1, 2, 7, UINT64_MAX}) {
      llvm::SmallVector<llvm::APInt, 3> Values{
          llvm::APInt(Width, V), llvm::APInt(Width, 3), llvm::APInt(Width, 5)};
      EXPECT_EQ(C.eval(R, Values), C.eval(Near, Values));
    }
  }
}

TEST(SymMBAComplementArithmetic, StorageRefusalDoesNotPoisonCompletedMemo) {
  SymContext C;
  const auto Parsed = parseSymExpr(C, "x*x*x*y+x*x*x*z", 257);
  ASSERT_TRUE(Parsed.ok());
  detail::WorkBudget Budget(65536);
  Budget.MemoOwner = &C;
  Budget.MemoCapacity = 1;
  MBAOptions Options;
  Options.VerifySamples = 0;
  Options.MaxTableBytes = 128;
  detail::SolveReport Refused;
  const size_t Nodes = C.numNodes();
  EXPECT_EQ(detail::solveArithmetic(C, Parsed.Root, Options, Budget, Refused),
            Parsed.Root);
  EXPECT_TRUE(Refused.BudgetExhausted);
  EXPECT_EQ(C.numNodes(), Nodes);
  Options.MaxTableBytes = MBAOptions{}.MaxTableBytes;
  detail::SolveReport Complete;
  const auto R =
      detail::solveArithmetic(C, Parsed.Root, Options, Budget, Complete);
  EXPECT_LT(C.readabilityCost(R), C.readabilityCost(Parsed.Root));
  EXPECT_FALSE(Complete.BudgetExhausted);
  EXPECT_EQ(Complete.Evidence, MBAEvidence::Derivation);
}

} // namespace
