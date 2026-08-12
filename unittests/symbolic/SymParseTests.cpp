//===- SymParseTests.cpp - Surface syntax and the round trip --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Covers the infix syntax and, most importantly, the identity that lets the
/// rest of the engine treat text as a faithful representation of a node:
///
///     parseSymExpr(Ctx, Ctx.toString(R), Ctx.width(R)).Root == R
///
/// The randomised case at the end of this file is the real test of it.  A
/// printer that drops a pair of brackets or a parser that mis-associates one
/// operator would produce an expression that still looks plausible, so nothing
/// short of comparing interned nodes over a large sample catches it.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymParse.h"

#include "gtest/gtest.h"

#include <random>

using namespace neverd::symbolic;

namespace {

constexpr uint32_t W32 = 32;

SymRef parse(SymContext &Ctx, llvm::StringRef Text, uint32_t Width = W32) {
  SymParseResult R = parseSymExpr(Ctx, Text, Width);
  if (!R.ok()) {
    ADD_FAILURE() << Text.str() << ": " << R.Error;
    // A failed parse leaves an invalid ref, which every accessor would index
    // out of bounds.  Hand back something inert so the assertion above is what
    // the reader sees rather than a crash further down.
    return Ctx.mkZero(Width);
  }
  return R.Root;
}

/// Assert that two spellings denote the same interned node.
void same(llvm::StringRef A, llvm::StringRef B, uint32_t Width = W32) {
  SymContext Ctx;
  EXPECT_EQ(parse(Ctx, A, Width), parse(Ctx, B, Width))
      << A.str() << "  vs  " << B.str();
}

std::string errorOf(llvm::StringRef Text, uint32_t Width = W32) {
  SymContext Ctx;
  SymParseResult R = parseSymExpr(Ctx, Text, Width);
  EXPECT_FALSE(R.ok()) << Text.str() << " unexpectedly parsed";
  return R.Error;
}

//===----------------------------------------------------------------------===//
// Precedence and associativity
//===----------------------------------------------------------------------===//

TEST(SymParse, PrecedenceFollowsC) {
  same("a | b & c", "a | (b & c)");
  same("a ^ b & c", "a ^ (b & c)");
  same("a | b ^ c", "a | (b ^ c)");
  same("a + b * c", "a + (b * c)");
  same("a * b + c", "(a * b) + c");
  same("a + b < c", "(a + b) < c");
  same("a < b == c < d", "(a < b) == (c < d)");
  same("a << b + c", "a << (b + c)");
  same("~a * b", "(~a) * b");
  same("-a + b", "(-a) + b");
  same("a && b || c", "(a && b) || c");
  same("a || b && c", "a || (b && c)");
}

TEST(SymParse, BinaryOperatorsAreLeftAssociativeAndTheTernaryIsRight) {
  same("a - b - c", "(a - b) - c");
  same("a / b / c", "(a / b) / c");
  same("a % b % c", "(a % b) % c");
  same("a << b << c", "(a << b) << c");
  same("a >> b >> c", "(a >> b) >> c");
  same("p ? q : r ? s : t", "p ? q : (r ? s : t)");
}

TEST(SymParse, SubtractionIsAdditionOfANegatedTerm) {
  SymContext Ctx;
  EXPECT_EQ(parse(Ctx, "a - b"), parse(Ctx, "a + -b"));
  EXPECT_EQ(parse(Ctx, "-a"), parse(Ctx, "0 - a"));
  EXPECT_EQ(parse(Ctx, "a - a"), parse(Ctx, "0"));
  EXPECT_EQ(parse(Ctx, "a + a"), parse(Ctx, "2 * a"));
  EXPECT_EQ(parse(Ctx, "a + (a << 1)"), parse(Ctx, "3 * a"));
}

//===----------------------------------------------------------------------===//
// Leaves
//===----------------------------------------------------------------------===//

TEST(SymParse, LiteralsAcceptEveryRadixAndWrapToTheParseWidth) {
  SymContext Ctx;
  EXPECT_EQ(parse(Ctx, "255"), Ctx.mkConst(W32, 255));
  EXPECT_EQ(parse(Ctx, "0xff"), Ctx.mkConst(W32, 255));
  EXPECT_EQ(parse(Ctx, "0XFF"), Ctx.mkConst(W32, 255));
  EXPECT_EQ(parse(Ctx, "0b11111111"), Ctx.mkConst(W32, 255));
  EXPECT_EQ(parse(Ctx, "0"), Ctx.mkZero(W32));

  // Values are taken modulo the width they are created at.
  EXPECT_EQ(parse(Ctx, "256", 8), Ctx.mkZero(8));
  EXPECT_EQ(parse(Ctx, "-1"), Ctx.mkOnes(W32));
  // A literal too wide for a machine word is still exact.
  EXPECT_EQ(parse(Ctx, "0x10000000000000000", 256),
            Ctx.mkConst(llvm::APInt(256, 1).shl(64)));
}

TEST(SymParse, AWidthSuffixOverridesTheParseWidth) {
  SymContext Ctx;
  EXPECT_EQ(Ctx.width(parse(Ctx, "255#8")), 8u);
  EXPECT_EQ(parse(Ctx, "255#8"), Ctx.mkConst(8, 255));
  EXPECT_EQ(parse(Ctx, "-1#8"), Ctx.mkOnes(8));
  EXPECT_EQ(Ctx.width(parse(Ctx, "x#64")), 64u);
  EXPECT_EQ(parse(Ctx, "x#64"), Ctx.mkVar("x", 64));
}

TEST(SymParse, IdentifiersCarryTheNamesADecompilerGenerates) {
  SymContext Ctx;
  EXPECT_EQ(parse(Ctx, "loc.4010a0"), Ctx.mkVar("loc.4010a0", W32));
  EXPECT_EQ(parse(Ctx, "arg$1"), Ctx.mkVar("arg$1", W32));
  EXPECT_EQ(parse(Ctx, "_v0"), Ctx.mkVar("_v0", W32));
}

//===----------------------------------------------------------------------===//
// Width handling
//===----------------------------------------------------------------------===//

TEST(SymParse, MixedWidthOperandsAreWidenedLikeC) {
  SymContext Ctx;
  SymRef E = parse(Ctx, "x#8 + y#32");
  EXPECT_EQ(Ctx.width(E), 32u);
  EXPECT_EQ(E, Ctx.mkAdd(Ctx.mkZExt(Ctx.mkVar("x", 8), W32),
                         Ctx.mkVar("y", W32)));
}

TEST(SymParse, AConditionThatIsNotOneBitIsComparedAgainstZero) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  EXPECT_EQ(parse(Ctx, "x ? 1 : 0"),
            Ctx.mkIte(Ctx.mkNe(X, Ctx.mkZero(W32)), Ctx.mkConst(W32, 1),
                      Ctx.mkZero(W32)));
  // `!` is logical and `~` is bitwise, so they part company above one bit.
  EXPECT_EQ(parse(Ctx, "!x"), Ctx.mkEq(X, Ctx.mkZero(W32)));
  EXPECT_EQ(parse(Ctx, "~x"), Ctx.mkNot(X));
  EXPECT_EQ(Ctx.width(parse(Ctx, "!x")), 1u);
  EXPECT_EQ(Ctx.width(parse(Ctx, "~x")), 32u);
}

//===----------------------------------------------------------------------===//
// Call forms
//===----------------------------------------------------------------------===//

TEST(SymParse, CallsCoverTheOperatorsCHasNoSpellingFor) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(parse(Ctx, "sdiv(x, y)"), Ctx.mkSDiv(X, Y));
  EXPECT_EQ(parse(Ctx, "srem(x, y)"), Ctx.mkSRem(X, Y));
  EXPECT_EQ(parse(Ctx, "ashr(x, y)"), Ctx.mkAShr(X, Y));
  EXPECT_EQ(parse(Ctx, "rol(x, y)"), Ctx.mkRol(X, Y));
  EXPECT_EQ(parse(Ctx, "slt(x, y)"), Ctx.mkSlt(X, Y));
  EXPECT_EQ(parse(Ctx, "sgt(x, y)"), Ctx.mkSlt(Y, X));
  EXPECT_EQ(parse(Ctx, "ite(x < y, x, y)"), Ctx.mkIte(Ctx.mkUlt(X, Y), X, Y));
  EXPECT_EQ(parse(Ctx, "x < y ? x : y"), Ctx.mkIte(Ctx.mkUlt(X, Y), X, Y));

  // The variadic forms are how a rewrite rule names a flattened operand list.
  EXPECT_EQ(parse(Ctx, "xor(x, y, x)"), Y);
  EXPECT_EQ(parse(Ctx, "add(x, y)"), Ctx.mkAdd(X, Y));
}

TEST(SymParse, BitCountsAreNotValuesInTheSurroundingWidth) {
  SymContext Narrow;
  // The `256` here has to mean 256 even though the ambient word is 8 bits
  // wide, which is why the counts are parsed as plain integers rather than as
  // expressions that would wrap to the surrounding width.
  SymRef E = parse(Narrow, "zext(x, 256)", 8);
  EXPECT_EQ(Narrow.width(E), 256u);
  EXPECT_EQ(E, Narrow.mkZExt(Narrow.mkVar("x", 8), 256));

  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  EXPECT_EQ(Ctx.width(parse(Ctx, "extract(x, 8, 16)")), 16u);
  EXPECT_EQ(parse(Ctx, "extract(x, 8, 16)"), Ctx.mkExtract(X, 8, 16));
  EXPECT_EQ(parse(Ctx, "trunc(x, 8)"), Ctx.mkExtract(X, 0, 8));
  EXPECT_EQ(Ctx.width(parse(Ctx, "concat(a#8, b#8, c#8)")), 24u);
}

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

TEST(SymParse, MalformedInputIsADiagnosticRatherThanACrash) {
  EXPECT_NE(errorOf("(a + b").find("expected ')'"), std::string::npos);
  EXPECT_NE(errorOf("a +").find("expected an expression"), std::string::npos);
  EXPECT_NE(errorOf("").find("expected an expression"), std::string::npos);
  EXPECT_NE(errorOf("a b").find("unexpected"), std::string::npos);
  EXPECT_NE(errorOf("bogus(a)").find("unknown function"), std::string::npos);
  EXPECT_NE(errorOf("not(a, b)").find("takes 1 argument"), std::string::npos);
  EXPECT_NE(errorOf("zext(x, 0)").find("width"), std::string::npos);
  EXPECT_NE(errorOf("zext(x#32, 8)").find("must not narrow"), std::string::npos);
  EXPECT_NE(errorOf("extract(x, 30, 8)").find("does not fit"), std::string::npos);
  EXPECT_NE(errorOf("a @ b").find("unexpected"), std::string::npos);

  // A name reused at a second width would trip mkVar's assertion, so the
  // parser has to catch it first.
  EXPECT_NE(errorOf("x#8 + x#16").find("already declared"), std::string::npos);

  // Deep nesting is bounded rather than left to overflow the stack.
  std::string Deep(4096, '(');
  Deep += "a";
  EXPECT_NE(errorOf(Deep).find("nests too deeply"), std::string::npos);
}

TEST(SymParse, ErrorOffsetPointsAtTheOffendingToken) {
  SymContext Ctx;
  SymParseResult R = parseSymExpr(Ctx, "a + bogus(b)", W32);
  ASSERT_FALSE(R.ok());
  EXPECT_EQ(R.ErrorOffset, 4u);
}

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

TEST(SymParse, PrintingPrefersTheShortSpelling) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(Ctx.toString(Ctx.mkSub(X, Y)), "x - y");
  EXPECT_EQ(Ctx.toString(Ctx.mkNeg(X)), "-x");
  EXPECT_EQ(Ctx.toString(Ctx.mkAdd(X, Ctx.mkConst(W32, 1))), "1 + x");
  EXPECT_EQ(Ctx.toString(Ctx.mkSub(X, Ctx.mkConst(W32, 1))), "-1 + x");
  EXPECT_EQ(Ctx.toString(Ctx.mkNot(X)), "~x");
  // A predicate is one bit wide, so its wider operands are the ones that leave
  // the ambient width and have to say so.  That is not noise: which width a
  // comparison happens at is not otherwise recoverable from the text.
  EXPECT_EQ(Ctx.toString(Ctx.mkNe(X, Y)), "x#32 != y#32");
  EXPECT_EQ(Ctx.toString(Ctx.mkOnes(W32)), "-1");
  // Small numbers read as quantities, large ones as masks.
  EXPECT_EQ(Ctx.toString(Ctx.mkConst(W32, 42)), "42");
  EXPECT_EQ(Ctx.toString(Ctx.mkConst(W32, 0xDEADBEEF)), "-0x21524111");
  EXPECT_EQ(Ctx.toString(Ctx.mkConst(W32, 0x7FFFFFFF)), "0x7FFFFFFF");
}

TEST(SymParse, PrintingBracketsExactlyWhereRegroupingWouldChangeMeaning) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);

  // Operands of a commutative operator come out in node order, which is why
  // the compound one trails here rather than leading.
  EXPECT_EQ(Ctx.toString(Ctx.mkMul(Ctx.mkAdd(X, Y), Z)), "z * (x + y)");
  EXPECT_EQ(Ctx.toString(Ctx.mkAnd(Ctx.mkOr(X, Y), Z)), "z & (x | y)");
  // `&` already binds tighter than `|`, so this needs nothing.
  EXPECT_EQ(Ctx.toString(Ctx.mkOr(Ctx.mkAnd(X, Y), Z)), "z | x & y");
  EXPECT_EQ(Ctx.toString(Ctx.mkNot(Ctx.mkAdd(X, Y))), "~(x + y)");

  // Division shares multiplication's strength without associating with it, so
  // a quotient inside a product cannot be left bare.
  EXPECT_EQ(Ctx.toString(Ctx.mkMul(Ctx.mkUDiv(X, Y), Z)), "z * (x / y)");
  EXPECT_EQ(Ctx.toString(Ctx.mkNeg(Ctx.mkUDiv(X, Y))), "-(x / y)");
  EXPECT_EQ(Ctx.toString(Ctx.mkUDiv(Ctx.mkNeg(X), Y)), "-x / y");
}

TEST(SymParse, PrintingAnnotatesOnlyTheLeavesThatLeaveTheAmbientWidth) {
  SymContext Ctx;
  SymRef X8 = Ctx.mkVar("x", 8);
  SymRef Y32 = Ctx.mkVar("y", W32);

  // Root width 32, so the 8-bit leaf is the one that needs saying.
  EXPECT_EQ(Ctx.toString(Ctx.mkAdd(Ctx.mkZExt(X8, W32), Y32)),
            "y + zext(x#8, 32)");
  // Root width 1: now the 32-bit operands are the exceptional ones.
  EXPECT_EQ(Ctx.toString(Ctx.mkUlt(Y32, Ctx.mkConst(W32, 10))), "y#32 < 10#32");
}

//===----------------------------------------------------------------------===//
// The round trip
//===----------------------------------------------------------------------===//

void expectRoundTrip(SymContext &Ctx, SymRef R) {
  std::string Text = Ctx.toString(R);
  SymParseResult Back = parseSymExpr(Ctx, Text, Ctx.width(R));
  ASSERT_TRUE(Back.ok()) << Text << ": " << Back.Error;
  EXPECT_EQ(Back.Root, R) << "printed as: " << Text
                          << "\n  reparsed as: " << Ctx.toString(Back.Root);
}

TEST(SymParse, HandWrittenShapesSurviveTheRoundTrip) {
  SymContext Ctx;
  static constexpr const char *Cases[] = {
      "x",
      "0x1234",
      "-1",
      "x + y",
      "x - y - z",
      "x * (y + z)",
      "(x ^ y) + 2 * (x & y)",
      "(x | y) - (x & y)",
      "~(x & y) | ~(x | y)",
      "x < y ? x - y : y - x",
      "a && b || !c",
      "x >> y << z",
      "ashr(sdiv(x, y), srem(x, y))",
      "rol(x, 3) ^ ror(y, 5)",
      "concat(extract(x, 0, 8), extract(x, 24, 8))",
      "zext(extract(x, 8, 8), 32) * 3",
      "sext(trunc(x, 16), 32)",
      "slt(x, y) ? sle(y, x) : x == y",
      "p#8 + q#8 == 0#8",
      "(x + 1) * (x + 2) * (x + 3)",
  };
  for (const char *C : Cases) {
    SymParseResult R = parseSymExpr(Ctx, C, W32);
    ASSERT_TRUE(R.ok()) << C << ": " << R.Error;
    expectRoundTrip(Ctx, R.Root);
  }
}

/// Builds random well-formed expressions at one width, so the round trip is
/// exercised over operator combinations nobody thought to write down.
class ExprGen {
public:
  ExprGen(SymContext &Ctx, uint32_t Width, uint64_t Seed)
      : Ctx(Ctx), Width(Width), Rng(Seed) {}

  SymRef word(unsigned Depth) {
    if (Depth == 0 || pick(5) == 0)
      return leaf();
    switch (pick(14)) {
    case 0:
      return Ctx.mkAdd(word(Depth - 1), word(Depth - 1));
    case 1:
      return Ctx.mkSub(word(Depth - 1), word(Depth - 1));
    case 2:
      return Ctx.mkMul(word(Depth - 1), word(Depth - 1));
    case 3:
      return Ctx.mkAnd(word(Depth - 1), word(Depth - 1));
    case 4:
      return Ctx.mkOr(word(Depth - 1), word(Depth - 1));
    case 5:
      return Ctx.mkXor(word(Depth - 1), word(Depth - 1));
    case 6:
      return Ctx.mkNot(word(Depth - 1));
    case 7:
      return Ctx.mkNeg(word(Depth - 1));
    case 8:
      return Ctx.mkShl(word(Depth - 1), word(Depth - 1));
    case 9:
      return Ctx.mkLShr(word(Depth - 1), word(Depth - 1));
    case 10:
      return Ctx.mkAShr(word(Depth - 1), word(Depth - 1));
    case 11:
      return Ctx.mkUDiv(word(Depth - 1), word(Depth - 1));
    case 12:
      return Ctx.mkRor(word(Depth - 1), word(Depth - 1));
    default:
      return Ctx.mkIte(bit(Depth - 1), word(Depth - 1), word(Depth - 1));
    }
  }

  SymRef bit(unsigned Depth) {
    switch (pick(6)) {
    case 0:
      return Ctx.mkEq(word(Depth), word(Depth));
    case 1:
      return Ctx.mkNe(word(Depth), word(Depth));
    case 2:
      return Ctx.mkUlt(word(Depth), word(Depth));
    case 3:
      return Ctx.mkUle(word(Depth), word(Depth));
    case 4:
      return Ctx.mkSlt(word(Depth), word(Depth));
    default:
      return Ctx.mkSle(word(Depth), word(Depth));
    }
  }

private:
  unsigned pick(unsigned N) { return unsigned(Rng() % N); }

  SymRef leaf() {
    if (pick(3) == 0) {
      // A spread of magnitudes, so both the decimal and the hex spellings and
      // the negative form all get exercised.
      static constexpr uint64_t Interesting[] = {
          0, 1, 2, 3, 7, 42, 255, 9999, 10000, 0x1234, 0xDEADBEEF, ~0ull};
      uint64_t V = Interesting[pick(std::size(Interesting))];
      return Ctx.mkConst(Width, V);
    }
    static constexpr const char *Names[] = {"a", "b", "c", "d", "e"};
    return Ctx.mkVar(Names[pick(std::size(Names))], Width);
  }

  SymContext &Ctx;
  uint32_t Width;
  std::mt19937_64 Rng;
};

TEST(SymParse, RandomExpressionsSurviveTheRoundTrip) {
  for (uint32_t Width : {1u, 8u, 32u, 64u, 256u}) {
    SymContext Ctx;
    ExprGen Gen(Ctx, Width, 0x5EED0000u + Width);
    for (unsigned I = 0; I < 400; ++I) {
      SymRef R = (I % 8 == 0) ? Gen.bit(3) : Gen.word(4);
      ASSERT_NO_FATAL_FAILURE(expectRoundTrip(Ctx, R))
          << "width " << Width << ", case " << I;
    }
  }
}

} // namespace
