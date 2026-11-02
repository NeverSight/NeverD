//===- SymConditionalControlTests.cpp - Branch-scoped rewrites ------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

using namespace neverd::symbolic;

namespace {

TEST(SymConditionalControls, RepeatedGuardsSpecializeOnlyTheirOwnArms) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1);
    auto A = C.mkVar("a", Width), B = C.mkVar("b", Width);
    auto D = C.mkVar("d", Width);
    auto Inner = C.mkIte(P, A, B);
    auto Root = C.mkIte(P, Inner, D);
    auto Want = C.mkIte(P, A, D);
    EXPECT_NE(Root, Want);
    EXPECT_EQ(simplifyMBADeep(C, Root).Expr, Want);
    auto Other = C.mkIte(Q, Inner, D);
    auto Reduced = simplifyMBADeep(C, Other).Expr;
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Other, Reduced)),
              neverd::solver::SatResult::Unsat);
    EXPECT_EQ(Proof.check(C.mkNe(Other, C.mkIte(Q, A, D))),
              neverd::solver::SatResult::Sat);
    // Specializing one use must not change a separately observed inner guard.
    EXPECT_EQ(C.operand(Inner, 0), P);
    EXPECT_EQ(C.operand(Inner, 2), B);
  }
}

TEST(SymConditionalControls, ConstantTagsRecoverThePredicate) {
  SymContext C;
  auto P = C.mkVar("p", 1);
  for (unsigned Width : {8u, 64u, 128u, 256u}) {
    auto A = C.mkConst(Width, 3), B = C.mkConst(Width, 7);
    auto Tagged = C.mkIte(P, A, B);
    EXPECT_EQ(simplifyMBADeep(C, C.mkEq(Tagged, A)).Expr, P);
    EXPECT_EQ(simplifyMBADeep(C, C.mkEq(Tagged, B)).Expr, C.mkNot(P));
    EXPECT_EQ(simplifyMBADeep(C, C.mkEq(Tagged, C.mkConst(Width, 5))).Expr,
              C.mkZero(1));
  }
}

TEST(SymConditionalControls, ArithmeticTagsRecoverThePredicate) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1);
    auto Bit = C.mkZExt(P, Width);
    auto Packed = C.mkMul(C.mkConst(Width, 64), Bit);
    auto Offset = C.mkAdd(C.mkConst(Width, 23), Packed);
    for (auto Root :
         {C.mkEq(Packed, C.mkZero(Width)), C.mkEq(Offset, C.mkConst(Width, 23)),
          C.mkUlt(Packed, C.mkConst(Width, 32))}) {
      auto Result = simplifyMBADeep(C, Root);
      EXPECT_EQ(Result.Expr, C.mkNot(P)) << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    }
    auto Other = C.mkVar("other", Width);
    auto Root = C.mkEq(Offset, Other);
    auto Result = simplifyMBADeep(C, Root);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
              neverd::solver::SatResult::Unsat);
    EXPECT_EQ(Proof.check(C.mkNe(Root, C.mkNot(P))),
              neverd::solver::SatResult::Sat);
  }
}

TEST(SymConditionalControls, LiteralXorEqualityUsesTheDecodedConstant) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Key = C.mkConst(Width, 0x093f959e2fcf0961);
    auto Tag = C.mkConst(Width, 0x4329f7b47e4d590c);
    for (auto Word : {X, C.mkXor(X, Y)}) {
      auto Encoded = C.mkXor(Word, Key);
      auto Root = C.mkEq(Encoded, Tag);
      auto Decoded = C.mkXor(Key, Tag);
      auto Want = Width == 1 ? (C.isConstZero(Decoded) ? C.mkNot(Word) : Word)
                             : C.mkEq(Word, Decoded);
      for (bool Negated : {false, true}) {
        auto Input = Negated ? C.mkNot(Root) : Root;
        auto Result = simplifyMBADeep(C, Input);
        EXPECT_EQ(Result.Expr, Negated ? C.mkNot(Want) : Want)
            << C.toString(Result.Expr);
        for (unsigned I = 0; I != 256; ++I) {
          llvm::APInt A(Width, I), B(Width, I * 37);
          if (I & 128)
            A.flipAllBits();
          auto Value = Word == X ? A : A ^ B;
          const bool Match = (Value ^ C.constValue(Key)) == C.constValue(Tag);
          EXPECT_EQ(C.eval(Result.Expr, {A, B}),
                    llvm::APInt(1, Negated ? !Match : Match));
        }
      }
    }
  }
}

TEST(SymConditionalControls, MaskedEqualityIsNotAnInvertibleXor) {
  SymContext C;
  auto X = C.mkVar("x", 8);
  auto Mask = C.mkConst(8, 0x35), Tag = C.mkConst(8, 0x10);
  for (auto Word : {C.mkAnd(X, Mask), C.mkOr(X, Mask)}) {
    auto Root = C.mkEq(Word, Tag);
    auto Result = simplifyMBADeep(C, Root);
    for (unsigned Input = 0; Input != 256; ++Input) {
      const auto Value = C.op(Word) == SymOp::And ? Input & 0x35 : Input | 0x35;
      EXPECT_EQ(C.evalU64(Result.Expr, {Input}), uint64_t(Value == 0x10));
    }
  }
}

TEST(SymConditionalControls, PartialBudgetsMatchAnIndependentTruthTable) {
  SymContext C;
  auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1);
  auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  auto Root =
      C.mkIte(P, C.mkIte(P, C.mkIte(Q, X, Y), Y), C.mkIte(P, X, C.mkAdd(X, Y)));
  for (unsigned Work : {0u, 1u, 3u, 16u, 64u, 10000u}) {
    MBAOptions Options;
    Options.MaxWork = Work;
    auto Result = simplifyMBADeep(C, Root, Options);
    EXPECT_LE(Result.Work, Work);
    if (!Work)
      EXPECT_EQ(Result.Expr, Root);
    for (unsigned A = 0; A != 16; ++A)
      for (unsigned B = 0; B != 16; ++B)
        for (unsigned Guard = 0; Guard != 4; ++Guard) {
          uint64_t Inputs[] = {Guard & 1u, Guard >> 1, A, B};
          unsigned Want = (Guard & 1) ? ((Guard & 2) ? A : B) : ((A + B) & 15);
          EXPECT_EQ(C.evalU64(Result.Expr, Inputs), Want);
        }
  }
}

TEST(SymConditionalControls, DataArmsAreNeverAssignedTheControlValue) {
  SymContext C;
  auto P = C.mkVar("p", 1), X = C.mkVar("x", 64);
  auto Root = C.mkIte(P, C.mkAdd(X, C.mkOne(64)), X);
  auto Result = simplifyMBADeep(C, Root).Expr;
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Result)),
            neverd::solver::SatResult::Unsat);
  EXPECT_EQ(Proof.check(C.mkNe(Result, X)), neverd::solver::SatResult::Sat);
}

} // namespace
