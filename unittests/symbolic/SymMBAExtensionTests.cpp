//===- SymMBAExtensionTests.cpp - Measuring widened bitwise regions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

using namespace neverd::symbolic;

namespace {
void checkByteInputs(SymContext &C, SymRef Input, SymRef Result) {
  SymEvalPlan Before(C, Input), After(C, Result);
  std::vector<uint64_t> Values(C.numVars(), 0);
  for (unsigned X = 0; X < 256; ++X)
    for (unsigned Y = 0; Y < 256; ++Y) {
      Values[0] = X;
      Values[1] = Y;
      if (Before.evalU64(Values) != After.evalU64(Values)) {
        ADD_FAILURE() << "widening changed the value at " << X << ", " << Y;
        return;
      }
    }
}

TEST(SymMBAExtension, RecoversWideCarriesWithoutMovingNarrowArithmetic) {
  for (unsigned Width : {1U, 3U, 8U, 32U, 64U, 128U})
    for (bool NarrowSum : {false, true}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(NarrowSum);
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto A = NarrowSum ? C.mkAdd(X, C.mkOne(Width)) : X;
      auto Left = C.mkZExt(A, Width * 2), Right = C.mkZExt(Y, Width * 2);
      auto Expected = C.mkAdd(Left, Right);
      auto Input =
          C.mkAdd(C.mkXor(Left, Right),
                  C.mkMul(C.mkConst(Width * 2, 2), C.mkAnd(Left, Right)));
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_LE(C.readability(Result.Expr), C.readability(Expected))
          << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      if (Width <= 8)
        checkByteInputs(C, Input, Result.Expr);
      std::vector<llvm::APInt> Values;
      for (unsigned I = 0; I < C.numVars(); ++I)
        Values.emplace_back(C.width(C.varRef(I)), 0);
      for (const auto &V :
           {llvm::APInt(Width, 0), llvm::APInt(Width, 1),
            llvm::APInt::getSignMask(Width), llvm::APInt::getAllOnes(Width)}) {
        Values[0] = V;
        Values[1] = ~V;
        EXPECT_EQ(C.eval(Input, Values), C.eval(Result.Expr, Values));
      }
    }
}

TEST(SymMBAExtension, SharedWideLeavesAndMasksRemainRelated) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto A = C.mkAnd(X, C.mkConst(8, 85));
  auto B = C.mkXor(Y, C.mkConst(8, 42));
  auto Left = C.mkZExt(A, 16), Right = C.mkZExt(B, 16);
  auto Input =
      C.mkSub(C.mkAdd(C.mkOr(Left, Right), C.mkAnd(Left, Right)), Left);
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Result = simplifyMBADeep(C, Input, Options);
  EXPECT_EQ(Result.Expr, Right) << C.toString(Result.Expr);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  checkByteInputs(C, Input, Result.Expr);
}

TEST(SymMBAExtension, ComplementsSignedValuesAndWorkLimitsKeepTheirBoundaries) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Complement = C.mkAdd(C.mkZExt(C.mkNot(X), 16), C.mkZExt(X, 16));
  auto Signed = C.mkSExt(X, 16), Unsigned = C.mkZExt(Y, 16);
  auto Mixed = C.mkAdd(C.mkXor(Signed, Unsigned),
                       C.mkMul(C.mkConst(16, 2), C.mkAnd(Signed, Unsigned)));
  for (auto Input : {Complement, Mixed}) {
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input, Options);
    checkByteInputs(C, Input, Result.Expr);
  }
  auto Left = C.mkZExt(X, 16), Right = C.mkZExt(Y, 16);
  auto Input = C.mkAdd(C.mkXor(Left, Right),
                       C.mkMul(C.mkConst(16, 2), C.mkAnd(Left, Right)));
  for (size_t Work : {0U, 1U, 16U}) {
    MBAOptions Options;
    Options.VerifySamples = 0;
    Options.MaxWork = Work;
    auto Result = simplifyMBADeep(C, Input, Options);
    EXPECT_LE(Result.Work, Work);
    if (!Work)
      EXPECT_EQ(Result.Expr, Input);
    checkByteInputs(C, Input, Result.Expr);
  }
}
} // namespace
