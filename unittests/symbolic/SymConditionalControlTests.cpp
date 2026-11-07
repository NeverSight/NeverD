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

TEST(SymConditionalControls, SelectedBooleanTagsKeepTheirOneBitRange) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1), R = C.mkVar("r", 1);
    auto Choice = C.mkIte(P, C.mkZero(Width),
                          C.mkIte(Q, C.mkZero(Width), C.mkZExt(R, Width)));
    auto Root = C.mkEq(C.mkMul(C.mkConst(-llvm::APInt(Width, 64)), Choice),
                       C.mkZero(Width));
    auto Result = simplifyMBADeep(C, Root);
    EXPECT_LT(C.readabilityCost(Result.Expr), C.readabilityCost(Root));
    EXPECT_EQ(C.toString(Result.Expr).find("mul"), std::string::npos)
        << C.toString(Result.Expr);
    auto Want = C.mkOr(P, C.mkOr(Q, C.mkNot(R)));
    neverd::solver::BitVectorSolver Proof(
        C, neverd::solver::SolverOptions::unlimited());
    EXPECT_EQ(Proof.check(C.mkNe(Result.Expr, Want)),
              neverd::solver::SatResult::Unsat);
    for (uint64_t PV = 0; PV != 2; ++PV)
      for (uint64_t QV = 0; QV != 2; ++QV)
        for (uint64_t RV = 0; RV != 2; ++RV)
          EXPECT_EQ(C.eval(Result.Expr, {llvm::APInt(1, PV), llvm::APInt(1, QV),
                                         llvm::APInt(1, RV)}),
                    llvm::APInt(1, PV || QV || !RV));
  }
}

TEST(SymConditionalControls, SelectionExtensionDoesNotTruncateOtherArms) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1), X = C.mkVar("x", 2);
    auto Choice = C.mkIte(P, C.mkZero(Width), C.mkZExt(X, Width));
    auto Scale = C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 1));
    auto Root = C.mkEq(C.mkMul(Scale, Choice), C.mkZero(Width));
    auto Result = simplifyMBADeep(C, Root).Expr;
    for (uint64_t PV = 0; PV != 2; ++PV)
      for (uint64_t XV = 0; XV != 4; ++XV)
        EXPECT_EQ(C.eval(Result, {llvm::APInt(1, PV), llvm::APInt(2, XV)}),
                  llvm::APInt(1, PV || !(XV & 1)));
    for (uint64_t Literal : {1u, 2u, 3u, 4u, 255u}) {
      auto Input = C.mkIte(P, C.mkZExt(X, Width), C.mkConst(Width, Literal));
      auto Reduced = simplifyMBADeep(C, Input).Expr;
      for (uint64_t PV = 0; PV != 2; ++PV)
        for (uint64_t XV = 0; XV != 4; ++XV)
          EXPECT_EQ(C.eval(Reduced, {llvm::APInt(1, PV), llvm::APInt(2, XV)}),
                    llvm::APInt(Width, PV ? XV : Literal));
    }
  }
}

TEST(SymConditionalControls, BooleanWordMasksRecoverTheirTwoDataArms) {
  for (unsigned Width : {8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1), X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Bit = C.mkZExt(P, Width);
    const SymRef Masks[] = {C.mkNeg(Bit), C.mkSub(Bit, C.mkOne(Width)),
                            C.mkSExt(P, Width)};
    for (unsigned I = 0; I != 3; ++I) {
      const auto Root = C.mkXor(X, C.mkAnd(Masks[I], C.mkXor(X, Y)));
      const auto Want = C.mkIte(P, I == 1 ? X : Y, I == 1 ? Y : X);
      const auto Result = simplifyMBADeep(C, Root);
      EXPECT_EQ(Result.Expr, Want) << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      neverd::solver::BitVectorSolver Proof(
          C, neverd::solver::SolverOptions::unlimited());
      EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
                neverd::solver::SatResult::Unsat);
      for (uint64_t PV = 0; PV != 2; ++PV)
        for (uint64_t A : {0ULL, 1ULL, 0x80ULL, 0xffULL, ~0ULL})
          for (uint64_t B : {0ULL, 1ULL, 0x80ULL, 0xffULL, ~0ULL})
            EXPECT_EQ(C.eval(Result.Expr, {llvm::APInt(1, PV),
                                           llvm::APInt(Width, A, false, true),
                                           llvm::APInt(Width, B, false, true)}),
                      llvm::APInt(Width, (bool(PV) != (I == 1)) ? B : A, false,
                                  true));
    }
  }
}

TEST(SymConditionalControls, PartialWordMasksAreNotBooleanSelections) {
  SymContext C;
  auto P = C.mkVar("p", 2), X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  for (auto Mask :
       {C.mkNeg(C.mkZExt(P, 8)), C.mkSub(C.mkZExt(P, 8), C.mkOne(8))}) {
    auto Root = C.mkXor(X, C.mkAnd(Mask, C.mkXor(X, Y)));
    auto Result = simplifyMBADeep(C, Root).Expr;
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Root, Result)),
              neverd::solver::SatResult::Unsat);
    for (uint64_t PV = 0; PV != 4; ++PV)
      for (uint64_t A = 0; A != 16; ++A)
        for (uint64_t B = 0; B != 16; ++B) {
          const uint64_t M =
              (Mask == C.mkNeg(C.mkZExt(P, 8)) ? 0u - PV : PV - 1) & 255;
          EXPECT_EQ(C.evalU64(Result, std::array<uint64_t, 3>{PV, A, B}),
                    A ^ (M & (A ^ B)));
        }
  }
}

TEST(SymConditionalControls, ScalarBooleanSelectionsKeepPredicateIdentity) {
  for (unsigned Width : {1u, 8u, 32u, 64u, 128u, 256u}) {
    SymContext C;
    const auto P = C.mkVar("p", 1);
    EXPECT_EQ(C.mkIte(P, C.mkOne(Width), C.mkZero(Width)), C.mkZExt(P, Width));
    EXPECT_EQ(C.mkIte(P, C.mkZero(Width), C.mkOne(Width)),
              C.mkZExt(C.mkNot(P), Width));
    if (Width == 1)
      continue;
    const auto Mask =
        C.mkIte(P, C.mkConst(llvm::APInt::getAllOnes(Width)), C.mkZero(Width));
    const auto Data = C.mkIte(P, C.mkConst(Width, 2), C.mkZero(Width));
    EXPECT_EQ(C.eval(Mask, {llvm::APInt(1, 0)}), llvm::APInt(Width, 0));
    EXPECT_EQ(C.eval(Mask, {llvm::APInt(1, 1)}),
              llvm::APInt::getAllOnes(Width));
    EXPECT_EQ(C.eval(Data, {llvm::APInt(1, 1)}), llvm::APInt(Width, 2));
  }
}
