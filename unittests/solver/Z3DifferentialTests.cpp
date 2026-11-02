//===- Z3DifferentialTests.cpp - Independent bitvector oracle -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#if defined(NEVERD_Z3_ORACLE) && NEVERD_Z3_ORACLE

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <z3++.h>

using namespace neverd::solver;
using namespace neverd::symbolic;

namespace {

constexpr SymOp Operators[] = {
    SymOp::Const, SymOp::Var,  SymOp::Add,     SymOp::Mul,    SymOp::And,
    SymOp::Or,    SymOp::Xor,  SymOp::Not,     SymOp::Shl,    SymOp::LShr,
    SymOp::AShr,  SymOp::UDiv, SymOp::SDiv,    SymOp::URem,   SymOp::SRem,
    SymOp::Rol,   SymOp::Ror,  SymOp::Extract, SymOp::Concat, SymOp::ZExt,
    SymOp::SExt,  SymOp::Ite,  SymOp::Eq,      SymOp::Ult,    SymOp::Ule,
    SymOp::Slt,   SymOp::Sle,
};

SymRef build(SymContext &C, SymOp Op, SymRef X, SymRef Y) {
  const unsigned W = C.width(X);
  switch (Op) {
  case SymOp::Const:
    return C.mkConst(W, 5);
  case SymOp::Var:
    return X;
  case SymOp::Add:
    return C.mkAdd(X, Y);
  case SymOp::Mul:
    return C.mkMul(X, Y);
  case SymOp::And:
    return C.mkAnd(X, Y);
  case SymOp::Or:
    return C.mkOr(X, Y);
  case SymOp::Xor:
    return C.mkXor(X, Y);
  case SymOp::Not:
    return C.mkNot(X);
  case SymOp::Shl:
    return C.mkShl(X, Y);
  case SymOp::LShr:
    return C.mkLShr(X, Y);
  case SymOp::AShr:
    return C.mkAShr(X, Y);
  case SymOp::UDiv:
    return C.mkUDiv(X, Y);
  case SymOp::SDiv:
    return C.mkSDiv(X, Y);
  case SymOp::URem:
    return C.mkURem(X, Y);
  case SymOp::SRem:
    return C.mkSRem(X, Y);
  case SymOp::Rol:
    return C.mkRol(X, Y);
  case SymOp::Ror:
    return C.mkRor(X, Y);
  case SymOp::Extract:
    return C.mkExtract(X, W > 2 ? 1 : 0, std::max(1u, W / 2));
  case SymOp::Concat:
    return C.mkConcat(X, Y);
  case SymOp::ZExt:
    return C.mkZExt(X, W + 3);
  case SymOp::SExt:
    return C.mkSExt(X, W + 3);
  case SymOp::Ite:
    return C.mkIte(C.mkExtract(Y, 0, 1), X, C.mkNot(X));
  case SymOp::Eq:
    return C.mkEq(X, Y);
  case SymOp::Ult:
    return C.mkUlt(X, Y);
  case SymOp::Ule:
    return C.mkUle(X, Y);
  case SymOp::Slt:
    return C.mkSlt(X, Y);
  case SymOp::Sle:
    return C.mkSle(X, Y);
  }
  llvm_unreachable("unhandled differential-test operator");
}

z3::expr numeral(z3::context &Z, const llvm::APInt &V) {
  llvm::SmallString<128> Text;
  V.toStringUnsigned(Text, 10);
  return Z.bv_val(Text.c_str(), V.getBitWidth());
}

z3::expr fitUnsigned(const z3::expr &V, unsigned W) {
  const unsigned Old = V.get_sort().bv_size();
  if (Old == W)
    return V;
  return Old < W ? z3::zext(V, W - Old) : V.extract(W - 1, 0);
}

// Construct the reference from the requested operation, never from NeverD's
// DAG or its Z3 adapter. Builder rewrites are therefore checked too.
z3::expr reference(SymOp Op, const z3::expr &X, const z3::expr &Y) {
  z3::context &Z = X.ctx();
  const unsigned W = X.get_sort().bv_size();
  auto predicate = [&](const z3::expr &P) {
    return z3::ite(P, Z.bv_val(1, 1), Z.bv_val(0, 1));
  };
  switch (Op) {
  case SymOp::Const:
    return Z.bv_val(5, W);
  case SymOp::Var:
    return X;
  case SymOp::Add:
    return X + Y;
  case SymOp::Mul:
    return X * Y;
  case SymOp::And:
    return X & Y;
  case SymOp::Or:
    return X | Y;
  case SymOp::Xor:
    return X ^ Y;
  case SymOp::Not:
    return ~X;
  case SymOp::Shl:
  case SymOp::LShr:
  case SymOp::AShr: {
    // Shift at a common width, then take the original value's low bits.
    // This keeps an independently sized count without copying the adapter's
    // truncation/overflow guards into the oracle.
    const unsigned Common = std::max(W, Y.get_sort().bv_size());
    z3::expr A =
        Op == SymOp::AShr ? z3::sext(X, Common - W) : z3::zext(X, Common - W);
    z3::expr B = fitUnsigned(Y, Common);
    z3::expr R = Op == SymOp::Shl    ? z3::shl(A, B)
                 : Op == SymOp::LShr ? z3::lshr(A, B)
                                     : z3::ashr(A, B);
    return R.extract(W - 1, 0);
  }
  case SymOp::UDiv:
    return z3::udiv(X, Y);
  case SymOp::SDiv:
    return X / Y;
  case SymOp::URem:
    return z3::urem(X, Y);
  case SymOp::SRem:
    return z3::srem(X, Y);
  case SymOp::Rol:
  case SymOp::Ror: {
    const unsigned Common = std::max(W, Y.get_sort().bv_size());
    z3::expr Residue = z3::urem(fitUnsigned(Y, Common), Z.bv_val(W, Common));
    z3::expr Count = fitUnsigned(Residue, W);
    return z3::expr(Z, Op == SymOp::Rol ? Z3_mk_ext_rotate_left(Z, X, Count)
                                        : Z3_mk_ext_rotate_right(Z, X, Count));
  }
  case SymOp::Extract: {
    unsigned Low = W > 2 ? 1 : 0;
    return X.extract(Low + std::max(1u, W / 2) - 1, Low);
  }
  case SymOp::Concat:
    return z3::concat(X, Y);
  case SymOp::ZExt:
    return z3::zext(X, 3);
  case SymOp::SExt:
    return z3::sext(X, 3);
  case SymOp::Ite:
    return z3::ite(Y.extract(0, 0) == Z.bv_val(1, 1), X, ~X);
  case SymOp::Eq:
    return predicate(X == Y);
  case SymOp::Ult:
    return predicate(z3::ult(X, Y));
  case SymOp::Ule:
    return predicate(z3::ule(X, Y));
  case SymOp::Slt:
    return predicate(X < Y);
  case SymOp::Sle:
    return predicate(X <= Y);
  }
  llvm_unreachable("unhandled reference operator");
}

llvm::APInt expected(z3::context &Z, SymOp Op, const llvm::APInt &A,
                     const llvm::APInt &B) {
  z3::expr R = reference(Op, numeral(Z, A), numeral(Z, B)).simplify();
  std::string Text;
  EXPECT_TRUE(R.is_numeral(Text)) << R;
  return llvm::APInt(R.get_sort().bv_size(), Text.empty() ? "0" : Text, 10);
}

using Sample = std::pair<llvm::APInt, llvm::APInt>;

std::vector<Sample> samples(unsigned W, bool Exhaustive = false) {
  std::vector<Sample> Out;
  if (Exhaustive) {
    for (unsigned A = 0; A < (1u << W); ++A)
      for (unsigned B = 0; B < (1u << W); ++B)
        Out.emplace_back(llvm::APInt(W, A), llvm::APInt(W, B));
    return Out;
  }
  llvm::APInt Zero(W, 0), One(W, 1), Ones = llvm::APInt::getAllOnes(W);
  llvm::APInt Min = llvm::APInt::getSignedMinValue(W);
  Out = {{Zero, Zero},
         {Ones, Zero},
         {Min, Ones},
         {Min, One},
         {One, Ones},
         {Ones, llvm::APInt(W, W)},
         {Min | One, llvm::APInt(W, W - 1)}};
  std::mt19937_64 Rng(0x49da32 + W);
  for (unsigned I = 0; I < 3; ++I) {
    llvm::APInt A(W, 0), B(W, 0);
    for (unsigned Bit = 0; Bit < W; Bit += 64) {
      A |= llvm::APInt(W, Rng()).shl(Bit);
      B |= llvm::APInt(W, Rng()).shl(Bit);
    }
    Out.emplace_back(std::move(A), std::move(B));
  }
  return Out;
}

void checkEvaluation(SymOp Op, unsigned W, unsigned AmountWidth,
                     llvm::ArrayRef<Sample> Inputs) {
  z3::context Z;
  SymContext C;
  SymRef X = C.mkVar("x", W), Y = C.mkVar("y", AmountWidth);
  SymRef E = build(C, Op, X, Y);
  SymEvalPlan Plan(C, E);
  for (const auto &[A, B] : Inputs) {
    SCOPED_TRACE(symOpName(Op));
    SCOPED_TRACE(W);
    SCOPED_TRACE(AmountWidth);
    llvm::SmallString<128> AText, BText;
    A.toStringUnsigned(AText, 16);
    B.toStringUnsigned(BText, 16);
    SCOPED_TRACE("x=0x" + AText.str().str() + ", y=0x" + BText.str().str());
    llvm::APInt Want = expected(Z, Op, A, B);
    const llvm::APInt Values[] = {A, B};
    EXPECT_EQ(Plan.eval(Values), Want);
    if (Plan.fitsU64()) {
      const uint64_t Values64[] = {A.getZExtValue(), B.getZExtValue()};
      EXPECT_EQ(Plan.evalU64(Values64), Want.getZExtValue());
    }
    SymRef Folded = build(C, Op, C.mkConst(A), C.mkConst(B));
    ASSERT_TRUE(C.isConst(Folded));
    EXPECT_EQ(C.constValue(Folded), Want);
  }
}

void checkSolvers(SymOp Op, unsigned W, unsigned AmountWidth,
                  llvm::ArrayRef<Sample> Inputs, bool AssertZ3Inputs = false) {
  z3::context Z;
  SymContext C;
  SymRef X = C.mkVar("x", W), Y = C.mkVar("y", AmountWidth);
  SymRef E = build(C, Op, X, Y);
  SymRef R = C.mkVar("result", C.width(E));
  SolverOptions Options;
  Options.Blast.MaxWidth = std::max({W, AmountWidth, C.width(E)});
  BitVectorSolver Builtin(C, Options);
  std::unique_ptr<Z3Solver> Adapter;
  ASSERT_TRUE(Builtin.assertEqual(R, E));
  if (!AssertZ3Inputs) {
    Adapter = std::make_unique<Z3Solver>(C);
    ASSERT_TRUE(Adapter->assertEqual(R, E));
  }
  for (const auto &[A, B] : Inputs) {
    SCOPED_TRACE(symOpName(Op));
    SCOPED_TRACE(W);
    SCOPED_TRACE(AmountWidth);
    llvm::SmallString<128> AText, BText;
    A.toStringUnsigned(AText, 16);
    B.toStringUnsigned(BText, 16);
    SCOPED_TRACE("x=0x" + AText.str().str() + ", y=0x" + BText.str().str());
    llvm::APInt Want = expected(Z, Op, A, B);
    SymRef Pins[] = {C.mkEq(X, C.mkConst(A)), C.mkEq(Y, C.mkConst(B))};
    ASSERT_EQ(Builtin.check(Pins), SatResult::Sat);
    if (AssertZ3Inputs) {
      // This tests wide operator semantics, not unconstrained query speed.
      // Ordinary assertions expose the concrete inputs to preprocessing before
      // Z3 expands a wide divider. Incremental assumptions are independently
      // covered by the exhaustive small-width cases and session tests.
      Adapter = std::make_unique<Z3Solver>(C);
      ASSERT_TRUE(Adapter->assertEqual(R, E));
      for (SymRef Pin : Pins)
        ASSERT_TRUE(Adapter->assertTrue(Pin));
    }
    ASSERT_EQ(AssertZ3Inputs ? Adapter->check() : Adapter->check(Pins),
              SatResult::Sat)
        << Adapter->reasonUnknown();
    auto ActualBuiltin = Builtin.model().value(C.varId(R));
    auto ActualAdapter = Adapter->model().value(C.varId(R));
    ASSERT_TRUE(ActualBuiltin.has_value());
    ASSERT_TRUE(ActualAdapter.has_value());
    EXPECT_EQ(*ActualBuiltin, Want);
    EXPECT_EQ(*ActualAdapter, Want);
  }
}

TEST(Z3Differential, EveryOperatorAtEverySmallInput) {
  for (unsigned W : {1u, 3u}) {
    std::vector<Sample> Inputs = samples(W, true);
    for (SymOp Op : Operators) {
      checkEvaluation(Op, W, W, Inputs);
      checkSolvers(Op, W, W, Inputs);
    }
  }
}

TEST(Z3Differential, WideConstantsAndEvaluators) {
  for (unsigned W : {8u, 32u, 64u, 128u, 256u}) {
    std::vector<Sample> Inputs = samples(W);
    for (SymOp Op : Operators)
      checkEvaluation(Op, W, W, Inputs);
  }
}

TEST(Z3Differential, WidePinnedSolverResults) {
  for (unsigned W : {128u, 256u}) {
    std::vector<Sample> Inputs = samples(W);
    for (SymOp Op : Operators)
      checkSolvers(Op, W, W, Inputs, /*AssertZ3Inputs=*/true);
  }
}

TEST(Z3Differential, IndependentlySizedShiftAndRotateCounts) {
  constexpr SymOp Shifts[] = {SymOp::Shl, SymOp::LShr, SymOp::AShr, SymOp::Rol,
                              SymOp::Ror};
  for (auto [W, AW] : {std::pair{3u, 1u},
                       {3u, 8u},
                       {8u, 16u},
                       {3u, 128u},
                       {128u, 8u},
                       {256u, 128u}}) {
    std::vector<Sample> Inputs;
    for (unsigned Bit : {0u, AW / 2, AW - 1}) {
      Inputs.emplace_back(llvm::APInt::getSignedMinValue(W) | llvm::APInt(W, 1),
                          llvm::APInt::getOneBitSet(AW, Bit));
    }
    Inputs.emplace_back(llvm::APInt(W, 1), llvm::APInt::getAllOnes(AW));
    for (SymOp Op : Shifts) {
      checkEvaluation(Op, W, AW, Inputs);
      checkSolvers(Op, W, AW, Inputs);
    }
  }
}

TEST(Z3Differential, ScaledProductsMatchIndependentFullWidthMultiplication) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 129u})
    for (unsigned Observed : {1u, 4u})
      for (bool Negative : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Observed << ':' << Negative);
        auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
        if (Negative)
          Scale = -Scale;
        const auto Mask = ~llvm::APInt(Width, 1);
        SymContext C;
        const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
        const auto Output = C.mkVar("output", Width);
        const auto Product = C.mkMul(
            {C.mkConst(Scale), C.mkAdd(X, Y), C.mkXor(X, C.mkConst(Mask))});
        Z3Solver Adapter(C);
        ASSERT_TRUE(Adapter.assertEqual(Output, Product));
        z3::context Z;
        z3::solver Reference(Z, "QF_BV");
        Reference.set("timeout", Z3SolverOptions{}.TimeoutMs);
        Reference.from_string(Adapter.dumpSMT2().c_str());
        const auto ZX = Z.bv_const("v0", Width);
        const auto ZY = Z.bv_const("v1", Width);
        const auto ZOutput = Z.bv_const("v2", Width);
        // The independent side uses ordinary full-width multiplication, with
        // no quotient or truncation and no pinned or sampled input values.
        const auto Expected =
            numeral(Z, Scale) * (ZX + ZY) * (ZX ^ numeral(Z, Mask));
        Reference.add(ZOutput != Expected);
        EXPECT_EQ(Reference.check(), z3::unsat) << Reference.reason_unknown();
      }
}

TEST(Z3Differential, DumpsReplayTheCurrentQueryWithoutStaleAssumptions) {
  SymContext C;
  SymRef X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  SymRef Seven = C.mkConst(8, 7);
  Z3Solver Adapter(C);
  ASSERT_TRUE(Adapter.assertEqual(X, Seven));

  auto replay = [&](z3::check_result Want) {
    const std::string Dump = Adapter.dumpSMT2();
    ASSERT_FALSE(Dump.empty());
    z3::context Z;
    z3::solver Replay(Z);
    Replay.from_string(Dump.c_str());
    EXPECT_EQ(Replay.check(), Want) << Dump;
  };

  ASSERT_EQ(Adapter.checkDistinct(X, Seven), SatResult::Unsat);
  replay(z3::unsat);
  ASSERT_EQ(Adapter.check(), SatResult::Sat);
  replay(z3::sat);

  // Changing assertions must drop the last one-shot disequality even before
  // another check, or a dump would replay an obsolete unsatisfiable query.
  ASSERT_EQ(Adapter.checkDistinct(X, Seven), SatResult::Unsat);
  ASSERT_TRUE(Adapter.assertEqual(Y, C.mkConst(8, 1)));
  replay(z3::sat);
  ASSERT_EQ(Adapter.check(), SatResult::Sat);

  SymRef WrongY = C.mkEq(Y, C.mkConst(8, 2));
  ASSERT_EQ(Adapter.check({WrongY}), SatResult::Unsat);
  replay(z3::unsat);
  ASSERT_TRUE(Adapter.assertTrue(C.mkUle(Y, Seven)));
  replay(z3::sat);
  ASSERT_EQ(Adapter.check(), SatResult::Sat);
}

} // namespace

#else

TEST(Z3Differential, OracleUnavailable) {
  GTEST_SKIP() << "The independent Z3 oracle was not enabled for this build";
}

#endif
