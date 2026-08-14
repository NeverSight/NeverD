//===- BitBlasterTests.cpp - Every operator against the evaluator ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks the circuit built for each operator against the expression
/// language's own evaluator, exhaustively, at a width small enough to
/// enumerate.
///
/// The shape of the check is what makes it worth having.  A variable is
/// constrained to equal the operator's result, the inputs are pinned to
/// concrete values by assumption, and the value the solver reports is compared
/// against what \c SymContext::eval computes for the same inputs.  So the
/// circuit is held to the semantics the rest of NeverD already agrees on,
/// rather than to a second copy of those semantics written here — which would
/// only prove the two copies were written by the same person.
///
/// Every case also exercises incremental solving, because the encoding is
/// built once and the enumeration runs as assumptions over it.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace neverd::solver;
using neverd::symbolic::SymContext;
using neverd::symbolic::SymRef;

namespace {

using UnaryBuilder = std::function<SymRef(SymContext &, SymRef)>;
using BinaryBuilder = std::function<SymRef(SymContext &, SymRef, SymRef)>;

/// Pin \p X (and \p Y, when it is valid) to concrete values, and compare the
/// result the solver reports for \p Result against the evaluator.
void checkPoint(const char *Name, SymContext &Ctx, BitVectorSolver &Solver,
                SymRef Expr, SymRef Result, SymRef X, uint64_t ValueX,
                SymRef Y, std::optional<uint64_t> ValueY) {
  llvm::SmallVector<SymRef, 2> Assumptions;
  Assumptions.push_back(Ctx.mkEq(X, Ctx.mkConst(Ctx.width(X), ValueX)));
  if (ValueY)
    Assumptions.push_back(Ctx.mkEq(Y, Ctx.mkConst(Ctx.width(Y), *ValueY)));

  ASSERT_EQ(Solver.check(Assumptions), SatResult::Sat)
      << Name << " at " << ValueX << "," << ValueY.value_or(0);

  const BitVectorModel &Model = Solver.model();
  std::optional<llvm::APInt> Got = Model.value(Ctx.varId(Result));
  ASSERT_TRUE(Got.has_value()) << Name;

  // The assumptions have to have taken effect, or the comparison below would
  // be checking the circuit against itself at some other point.
  std::optional<llvm::APInt> ModelX = Model.value(Ctx.varId(X));
  ASSERT_TRUE(ModelX.has_value()) << Name;
  ASSERT_EQ(ModelX->getZExtValue(), ValueX) << Name;

  llvm::APInt Expected = Ctx.eval(Expr, Model.asVarValues(Ctx));
  EXPECT_EQ(Got->getZExtValue(), Expected.getZExtValue())
      << Name << " at " << ValueX << "," << ValueY.value_or(0);
}

void checkUnary(const char *Name, uint32_t Width, const UnaryBuilder &Build) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Expr = Build(Ctx, X);
  SymRef Result = Ctx.mkVar("r", Ctx.width(Expr));

  BitVectorSolver Solver(Ctx);
  ASSERT_TRUE(Solver.assertEqual(Result, Expr)) << Name;

  for (uint64_t A = 0, End = uint64_t(1) << Width; A < End; ++A)
    checkPoint(Name, Ctx, Solver, Expr, Result, X, A, SymRef(), std::nullopt);
}

void checkBinary(const char *Name, uint32_t Width, const BinaryBuilder &Build) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Y = Ctx.mkVar("y", Width);
  SymRef Expr = Build(Ctx, X, Y);
  SymRef Result = Ctx.mkVar("r", Ctx.width(Expr));

  BitVectorSolver Solver(Ctx);
  ASSERT_TRUE(Solver.assertEqual(Result, Expr)) << Name;

  const uint64_t End = uint64_t(1) << Width;
  for (uint64_t A = 0; A < End; ++A)
    for (uint64_t B = 0; B < End; ++B)
      checkPoint(Name, Ctx, Solver, Expr, Result, X, A, Y, B);
}

constexpr uint32_t W4 = 4;
/// A width that is not a power of two, which is where a rotate stops being a
/// selection of the amount's low bits and a shift's guard against overrunning
/// the width starts to matter.
constexpr uint32_t W3 = 3;

TEST(BitBlaster, Arithmetic) {
  checkBinary("add", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkAdd(X, Y); });
  checkBinary("sub", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkSub(X, Y); });
  checkBinary("mul", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkMul(X, Y); });
  checkUnary("neg", W4, [](SymContext &C, SymRef X) { return C.mkNeg(X); });
}

TEST(BitBlaster, Bitwise) {
  checkBinary("and", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkAnd(X, Y); });
  checkBinary("or", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkOr(X, Y); });
  checkBinary("xor", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkXor(X, Y); });
  checkUnary("not", W4, [](SymContext &C, SymRef X) { return C.mkNot(X); });
}

TEST(BitBlaster, ShiftsByAnUnknownAmount) {
  checkBinary("shl", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkShl(X, Y); });
  checkBinary("lshr", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkLShr(X, Y); });
  checkBinary("ashr", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkAShr(X, Y); });

  // At a width that is not a power of two an amount can name a shift that is
  // representable and still past the end, which is the case the staged shifts
  // do not cover on their own.
  checkBinary("shl.w3", W3,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkShl(X, Y); });
  checkBinary("lshr.w3", W3,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkLShr(X, Y); });
  checkBinary("ashr.w3", W3,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkAShr(X, Y); });
}

TEST(BitBlaster, RotatesByAnUnknownAmount) {
  checkBinary("rol", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkRol(X, Y); });
  checkBinary("ror", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkRor(X, Y); });

  // Here the amount has to be reduced modulo the width by division rather than
  // by taking its low bits.
  checkBinary("rol.w3", W3,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkRol(X, Y); });
  checkBinary("ror.w3", W3,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkRor(X, Y); });
}

TEST(BitBlaster, DivisionIncludingTheTotalisedCases) {
  // Zero divisors and the most negative value over minus one are inside these
  // enumerations, so the boundary behaviour the expression language defines is
  // covered rather than assumed.
  checkBinary("udiv", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkUDiv(X, Y); });
  checkBinary("urem", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkURem(X, Y); });
  checkBinary("sdiv", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkSDiv(X, Y); });
  checkBinary("srem", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkSRem(X, Y); });
}

TEST(BitBlaster, Predicates) {
  checkBinary("eq", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkEq(X, Y); });
  checkBinary("ult", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkUlt(X, Y); });
  checkBinary("ule", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkUle(X, Y); });
  checkBinary("slt", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkSlt(X, Y); });
  checkBinary("sle", W4,
              [](SymContext &C, SymRef X, SymRef Y) { return C.mkSle(X, Y); });
}

TEST(BitBlaster, StructuralOperators) {
  checkBinary("concat", W4, [](SymContext &C, SymRef X, SymRef Y) {
    return C.mkConcat(X, Y);
  });
  checkBinary("ite", W4, [](SymContext &C, SymRef X, SymRef Y) {
    return C.mkIte(C.mkUlt(X, Y), X, Y);
  });
  checkUnary("zext", W4,
             [](SymContext &C, SymRef X) { return C.mkZExt(X, 8); });
  checkUnary("sext", W4,
             [](SymContext &C, SymRef X) { return C.mkSExt(X, 8); });
  checkUnary("extract", W4,
             [](SymContext &C, SymRef X) { return C.mkExtract(X, 1, 2); });
}

TEST(BitBlaster, MixedExpressionsAgreeWithTheEvaluator) {
  // Nothing here is a single operator: the point is that the operators compose
  // through the shared cache without a node's bits being reused at the wrong
  // width or the wrong offset.
  checkBinary("mixed.arith", W4, [](SymContext &C, SymRef X, SymRef Y) {
    return C.mkAdd(C.mkMul(X, Y), C.mkSub(X, C.mkNot(Y)));
  });
  checkBinary("mixed.structural", W4, [](SymContext &C, SymRef X, SymRef Y) {
    SymRef Wide = C.mkConcat(C.mkZExt(X, 8), C.mkSExt(Y, 8));
    return C.mkExtract(C.mkLShr(Wide, C.mkConst(16, 3)), 2, 4);
  });
  checkBinary("mixed.predicate", W4, [](SymContext &C, SymRef X, SymRef Y) {
    return C.mkIte(C.mkSlt(X, Y), C.mkUDiv(X, Y), C.mkURem(Y, X));
  });
}

TEST(BitBlaster, ProductsByEveryLiteralAgreeWithTheEvaluator) {
  // A literal multiplier is recoded into shifted additions and subtractions
  // rather than encoded as a multiplier, and how it recodes depends on the runs
  // of set bits in it — a run of one or two bits is added copy by copy, and a
  // longer one becomes a subtraction from the copy above it.  Every literal at
  // this width is tried so that no run length goes unexercised, including the
  // run that reaches the top bit and so has nothing above it to subtract from.
  constexpr uint32_t Width = 5;
  for (uint64_t K = 0, End = uint64_t(1) << Width; K < End; ++K) {
    std::string Name = "mul.by." + std::to_string(K);
    checkUnary(Name.c_str(), Width, [K](SymContext &C, SymRef X) {
      return C.mkMul(X, C.mkConst(Width, K));
    });
  }
}

/// Gates the blaster builds for one expression, which is the cost model
/// everything above it is budgeted against.
size_t gateCost(uint32_t Width, const BinaryBuilder &Build) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef Y = Ctx.mkVar("y", Width);

  SatSolver Sat;
  CnfEncoder Enc(Sat);
  BitBlaster Blaster(Ctx, Enc);
  BitLits Bits;
  EXPECT_TRUE(Blaster.blast(Build(Ctx, X, Y), Bits));
  return Enc.numGates();
}

TEST(BitBlaster, AProductByALiteralIsNotAMultiplier) {
  constexpr uint32_t Width = 32;

  const size_t Sum =
      gateCost(Width, [](SymContext &C, SymRef X, SymRef Y) {
        return C.mkAdd(X, Y);
      });
  const size_t Product =
      gateCost(Width, [](SymContext &C, SymRef X, SymRef Y) {
        return C.mkMul(X, Y);
      });
  ASSERT_GT(Product, 8 * Sum);

  // A subtraction is spelled as a multiplication by minus one, so it arrives at
  // the multiplier like any other product.  Encoding it as one would make every
  // subtraction in a recovered expression cost an adder row per bit, which is
  // the difference between a query that answers and one that does not.
  const size_t Difference =
      gateCost(Width, [](SymContext &C, SymRef X, SymRef Y) {
        return C.mkSub(X, Y);
      });
  EXPECT_LT(Difference, 3 * Sum);

  // A negation is one carry chain, and a carry chain with a constant operand is
  // cheaper than an addition rather than dearer.
  const size_t Negation =
      gateCost(Width, [](SymContext &C, SymRef X, SymRef) {
        return C.mkNeg(X);
      });
  EXPECT_LE(Negation, Sum);
}

TEST(BitBlaster, SharedSubtermsAreEncodedOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 16);
  SymRef Y = Ctx.mkVar("y", 16);
  SymRef Shared = Ctx.mkMul(X, Y);

  BitVectorSolver Solver(Ctx);
  ASSERT_TRUE(Solver.assertTrue(Ctx.mkUlt(Shared, Ctx.mkConst(16, 100))));
  size_t AfterFirst = Solver.sat().numVars();

  // The second constraint names the same product.  Interning made it the same
  // node, and the blaster's cache has to turn that into the same literals
  // rather than a second multiplier.
  ASSERT_TRUE(Solver.assertTrue(Ctx.mkUlt(Ctx.mkConst(16, 10), Shared)));
  size_t AfterSecond = Solver.sat().numVars();

  EXPECT_LT(AfterSecond - AfterFirst, AfterFirst / 4);
  EXPECT_EQ(Solver.check(), SatResult::Sat);
}

TEST(BitBlaster, AWidthBeyondTheLimitIsRefusedRatherThanEncoded) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);

  SolverOptions Opts;
  Opts.Blast.MaxWidth = 8;

  BitVectorSolver Solver(Ctx, Opts);
  EXPECT_FALSE(Solver.assertTrue(Ctx.mkUlt(X, Ctx.mkConst(64, 5))));
  EXPECT_FALSE(Solver.ok());
  EXPECT_EQ(Solver.encodeError(), BlastError::WidthTooLarge);
  // A formula that was never fully encoded constrains nothing, so no answer
  // may be given for it.
  EXPECT_EQ(Solver.check(), SatResult::Unknown);
}

} // namespace
