//===- SourceValueTests.cpp - Source Value tests -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source Value tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceValues.h"

#include <cfenv>
#include <cmath>
#include <cstring>

namespace {
using namespace neverd::web;

struct Evaluated {
  SourceAnalysis Source;
  SourceValueAnalysis Values;
  uint32_t Root = UINT32_MAX;
  explicit Evaluated(std::string_view Expression) {
    const auto Text = "const result = (" + std::string(Expression) + ");";
    Source = inspectJavaScript(identity("value-test", {Text}), Text, "script");
    EXPECT_EQ(Source.ParseStatus, "parsed") << Expression;
    Values = analyzeSourceValues(Source);
    for (const auto &N : Source.Nodes)
      if (N.Kind == "VariableDeclarator")
        for (const auto &C : N.Children)
          if (C.Field == "init")
            Root = C.Index;
  }
  SourceValue root() const {
    if (Root < Values.Nodes.size())
      return Values.Nodes[Root];
    ADD_FAILURE() << "Missing expression value: " << Values.Status;
    return {};
  }
};

SourceValue evaluate(std::string_view Expression) {
  return Evaluated(Expression).root();
}
uint64_t bits(double D) {
  uint64_t B;
  std::memcpy(&B, &D, sizeof(B));
  return B;
}
void expectNumber(std::string_view Expression, double Expected) {
  const auto R = evaluate(Expression);
  ASSERT_EQ(R.Status, "constant") << Expression << ": " << R.Reason;
  ASSERT_EQ(R.Value->Kind, PrimitiveKind::Number);
  EXPECT_EQ(R.Value->NumberBits, bits(Expected)) << Expression;
}
void expectString(std::string_view Expression, std::u16string_view Expected) {
  const auto R = evaluate(Expression);
  ASSERT_EQ(R.Status, "constant") << Expression << ": " << R.Reason;
  ASSERT_EQ(R.Value->Kind, PrimitiveKind::String);
  EXPECT_EQ(R.Value->String, Expected) << Expression;
}
void expectBoolean(std::string_view Expression, bool Expected) {
  const auto R = evaluate(Expression);
  ASSERT_EQ(R.Status, "constant") << Expression << ": " << R.Reason;
  ASSERT_EQ(R.Value->Kind, PrimitiveKind::Boolean);
  EXPECT_EQ(R.Value->Boolean, Expected) << Expression;
}
void expectBigInt(std::string_view Expression, std::string_view Expected) {
  const auto R = evaluate(Expression);
  ASSERT_EQ(R.Status, "constant") << Expression << ": " << R.Reason;
  ASSERT_EQ(R.Value->Kind, PrimitiveKind::BigInt);
  EXPECT_EQ(R.Value->BigInt, Expected) << Expression;
}

TEST(WebSourceValues, Binary64KeepsZerosNanAndTruncatingRemainders) {
  expectNumber("-0", -0.0);
  expectNumber("-4 % 2", -0.0);
  expectNumber("5.5 % 2", 1.5);
  expectNumber("-5.5 % 2", -1.5);
  expectNumber("1 / -0", -INFINITY);
  expectNumber("1e308 * 1e308", INFINITY);
  expectNumber("5e-324 / 2", 0.0);
  expectNumber("(9007199254740992 + 1) - 9007199254740992", 0.0);
  const auto NaN = evaluate("0 / 0");
  ASSERT_EQ(NaN.Status, "constant");
  EXPECT_EQ(NaN.Value->NumberBits, UINT64_C(0x7ff8000000000000));
  expectBoolean("(0 / 0) === (0 / 0)", false);
  expectBoolean("0 === -0", true);
  expectBoolean("(0 / 0) <= 1", false);
  expectBoolean("(0 / 0) >= 1", false);
}

TEST(WebSourceValues, ArithmeticDoesNotInheritHostRoundingMode) {
  Evaluated E("0.1 + 0.2");
  Evaluated Formatted("'' + 0.1");
  ASSERT_EQ(E.root().Status, "constant");
  const auto Expected = E.root().Value->NumberBits;
  const auto Original = std::fegetround();
  ASSERT_NE(Original, -1);
  struct RestoreRounding {
    int Mode;
    ~RestoreRounding() { std::fesetround(Mode); }
  } Restore{Original};
  const auto Changed = std::fesetround(FE_DOWNWARD);
  if (Changed == 0) {
    const auto Down = analyzeSourceValues(E.Source);
    ASSERT_EQ(Down.Nodes.size(), E.Source.Nodes.size());
    ASSERT_NE(Down.Nodes[E.Root].Value, nullptr);
    EXPECT_EQ(Down.Nodes[E.Root].Value->NumberBits, Expected);
    const auto Conversion = analyzeSourceValues(Formatted.Source);
    EXPECT_EQ(Conversion.Status, "unsupported");
    EXPECT_TRUE(Conversion.Nodes.empty());
    ASSERT_FALSE(Conversion.Diagnostics.empty());
    EXPECT_EQ(Conversion.Diagnostics[0].Code, "unsupported_host_rounding_mode");
    const auto Parse = inspectJavaScript("rounding-test", "0.1;", "script");
    EXPECT_EQ(Parse.ParseStatus, "unsupported_environment");
    EXPECT_TRUE(Parse.Nodes.empty());
    EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
  }
  EXPECT_EQ(std::fesetround(Original), 0);
}

TEST(WebSourceValues, NumericCoercionUsesJavaScriptGrammarAndWhitespace) {
  expectNumber("+''", 0);
  expectNumber("+'\\uFEFF\\u2000-0\\n'", -0.0);
  expectNumber("+'0x10'", 16);
  expectNumber("+'0b101'", 5);
  expectNumber("+'0o10'", 8);
  expectNumber("+'08'", 8);
  expectNumber("+'+.5e1'", 5);
  expectNumber("+true + null", 1);
  for (const auto Text : {"+'-0x10'", "+'0x'", "+'1_000'", "+'inf'", "+'0x1p2'",
                          "+'\\u0085'", "+'1\\0'", "+'1e'"}) {
    const auto R = evaluate(Text);
    ASSERT_EQ(R.Status, "constant") << Text;
    EXPECT_EQ(R.Value->NumberBits, UINT64_C(0x7ff8000000000000)) << Text;
  }
}

TEST(WebSourceValues, ShiftsAndBitwiseCoerceWithoutHostSignedOverflow) {
  expectNumber("4294967295 | 0", -1);
  expectNumber("-1 >>> 0", 4294967295.0);
  expectNumber("1 << 31", -2147483648.0);
  expectNumber("-2147483648 >> 31", -1);
  expectNumber("1 << -1", -2147483648.0);
  expectNumber("1 << 32", 1);
  expectNumber("4294967297.9 & 3", 1);
  expectNumber("~(1 / 0)", -1);
  expectNumber("1e100 | 0", 0);
  expectNumber("-0.5 | 0", 0);
}

TEST(WebSourceValues, Utf16ConcatenationAndPrimitiveFormattingAreExact) {
  expectString("'\\ud800' + '\\0' + '\\udfff'",
               std::u16string{0xd800, 0, 0xdfff});
  expectString("'x' + (-0) + true + null + (void 0)", u"x0truenullundefined");
  expectString("'' + 1e20", u"100000000000000000000");
  expectString("'' + 1e21", u"1e+21");
  expectString("'' + 1e-6", u"0.000001");
  expectString("'' + 1e-7", u"1e-7");
  expectString("`${1 + 2}:${true}:${null}:${void 0}`",
               u"3:true:null:undefined");
  expectString("typeof null", u"object");
  expectString("typeof (void 1)", u"undefined");
  expectBoolean("'\\ud800' < '\\ue000'", true);
}

TEST(WebSourceValues, ShortCircuitSkipsUnselectedCallsAndExceptions) {
  expectNumber("true ? 7 : (1n / 0n)", 7);
  expectBoolean("false && missing()", false);
  expectNumber("0 ?? missing()", 0);
  expectString("'' || 'fallback'", u"fallback");
  expectNumber("null ?? 2", 2);
  expectNumber("(1, 2, 3)", 3);
  EXPECT_EQ(evaluate("unknown ? 1 : 1").Status, "unknown");
  EXPECT_EQ(evaluate("(missing(), 3)").Status, "unknown");
  EXPECT_EQ(evaluate("unknown + (1n / 0n)").Status, "unknown");
  EXPECT_EQ(evaluate("1 + (1n / 0n)").Status, "would_throw");
}

TEST(WebSourceValues, BigIntsHaveBoundedExactArithmeticAndDistinctErrors) {
  expectBigInt("0xffn + 0b10n + 0o7n + 1_000n", "1264");
  expectBigInt("-7n / 3n", "-2");
  expectBigInt("-7n % 3n", "-1");
  expectBigInt("~1n", "-2");
  expectBigInt("(-1n & 255n) ^ 15n", "240");
  expectBigInt("16n << -2n", "4");
  expectBigInt("-3n >> 1n", "-2");
  expectBigInt("2n ** 100n", "1267650600228229401496703205376");
  expectBigInt("-1n >> 100000000000000000000n", "-1");
  expectBigInt("0n << 100000000000000000000n", "0");
  expectBigInt("(-1n) ** 100000000000000000001n", "-1");
  expectString("'id=' + 255n", u"id=255");
  for (const auto Text :
       {"+1n", "1n + 1", "1n / 0n", "1n % 0n", "1n >>> 0n", "2n ** -1n"}) {
    EXPECT_EQ(evaluate(Text).Status, "would_throw") << Text;
  }
  EXPECT_EQ(evaluate("2n ** 1024n").Status, "budget_exceeded");
  EXPECT_EQ(evaluate("1n << 1024n").Status, "budget_exceeded");
}

TEST(WebSourceValues, EqualityDoesNotCollapseTypesOrClaimUnqualifiedResults) {
  expectBoolean("1 === true", false);
  expectBoolean("1 == true", true);
  expectBoolean("1 == '1'", true);
  expectBoolean("null == (void 0)", true);
  expectBoolean("null == 0", false);
  expectBoolean("1n === 1", false);
  expectBoolean("1n == 1n", true);
  expectBoolean("1n < 2n", true);
  expectBoolean("'2' < '10'", false);
  EXPECT_EQ(evaluate("1n == 1").Status, "unsupported");
  EXPECT_EQ(evaluate("1n < '2'").Status, "unsupported");
  EXPECT_EQ(evaluate("2 ** 5").Status, "unsupported");
  EXPECT_EQ(evaluate("1 in 2").Status, "would_throw");
  EXPECT_EQ(evaluate("1 instanceof 2").Status, "would_throw");
}

TEST(WebSourceValues, NoNameOrObjectCanCauseTargetEvaluation) {
  for (const auto Text : {"undefined", "NaN", "Infinity", "eval('1 + 1')",
                          "Math.max(1, 2)", "String(1)", "Number('2')",
                          "'a'.length", "({valueOf() {while(true){}}}) + 1",
                          "(()=>{while(true){}})()", "void missing()"}) {
    EXPECT_EQ(evaluate(Text).Status, "unknown") << Text;
  }
  const std::string Text = "const known = 1; known + 1;";
  const auto S = inspectJavaScript("test", Text, "script");
  const auto V = analyzeSourceValues(S);
  for (size_t I = 0; I < S.Nodes.size(); ++I)
    if (S.Nodes[I].Kind == "BinaryExpression")
      EXPECT_EQ(V.Nodes[I].Status, "unknown");
}

TEST(WebSourceValues, ValueAndAggregateBudgetsRefuseWithoutPartialConstants) {
  const auto Long =
      evaluate("'" + std::string(MaxJavaScriptValueStringUnits + 1, 's') + "'");
  EXPECT_EQ(Long.Status, "budget_exceeded");
  EXPECT_FALSE(Long.Value);
  std::string Text;
  for (unsigned N = 0; N < 8; ++N) {
    Text += "const x" + std::to_string(N) + " = ''";
    for (unsigned I = 0; I < 32; ++I)
      Text += "+'" + std::string(1024, 'a') + "'";
    Text += ";";
  }
  const auto S = inspectJavaScript("budget", Text, "script");
  ASSERT_EQ(S.ParseStatus, "parsed");
  const auto V = analyzeSourceValues(S);
  EXPECT_EQ(V.Status, "budget_exceeded");
  EXPECT_TRUE(V.Nodes.empty());
  EXPECT_LE(V.Steps, MaxJavaScriptValueSteps);
  EXPECT_LE(V.AllocatedStringUnits, MaxJavaScriptValueAllocatedUnits);
  ASSERT_FALSE(V.Diagnostics.empty());
}

TEST(WebSourceValues, StableIdentitiesAndMalformedModelAdmission) {
  Evaluated E("1 + 2");
  EXPECT_EQ(E.Values.ID, analyzeSourceValues(E.Source).ID);
  E.Source.Nodes[0].Children[0].Index = 0;
  EXPECT_THROW(analyzeSourceValues(E.Source), Error);
  SourceAnalysis Missing;
  EXPECT_EQ(analyzeSourceValues(Missing).Status, "unavailable");
}
} // namespace
