#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceView.h"

#include <algorithm>

namespace {
using namespace neverd::web;

SourceAnalysis source(std::string_view Text, std::string_view Type = "script") {
  return inspectJavaScript(identity("view-test", {Text}), Text, Type);
}

SourceView view(std::string_view Text, std::string_view Options = {}) {
  const auto S = source(Text);
  EXPECT_EQ(S.ParseStatus, "parsed");
  EXPECT_EQ(S.LexemeStatus, "ok");
  return makeSourceView(S, analyzeSourceBindings(S), Text,
                        sourceViewPolicy(Options));
}

std::string review(uint64_t Start, uint64_t Length) {
  return "{\"schema_version\":1,\"locally_reviewed\":true,\"reviewed_ranges\":["
         "{\"byte_offset\":\"" +
         std::to_string(Start) + "\",\"byte_length\":\"" +
         std::to_string(Length) + "\"}]}";
}

void coverage(const SourceView &V, std::string_view Original) {
  uint64_t SourceAt = 0, ViewAt = 0;
  for (const auto &S : V.Segments) {
    EXPECT_EQ(S.SourceStart, SourceAt);
    EXPECT_EQ(S.Start, ViewAt);
    EXPECT_LT(S.SourceStart, S.SourceEnd);
    EXPECT_LT(S.Start, S.End);
    EXPECT_TRUE(sourceByteBoundary(Original, S.SourceStart));
    EXPECT_TRUE(sourceByteBoundary(Original, S.SourceEnd));
    EXPECT_TRUE(sourceByteBoundary(V.Text, S.Start));
    EXPECT_TRUE(sourceByteBoundary(V.Text, S.End));
    if (S.Identity)
      EXPECT_EQ(Original.substr(S.SourceStart, S.SourceEnd - S.SourceStart),
                V.Text.substr(S.Start, S.End - S.Start));
    SourceAt = S.SourceEnd;
    ViewAt = S.End;
  }
  EXPECT_EQ(SourceAt, Original.size());
  EXPECT_EQ(ViewAt, V.Text.size());
  EXPECT_EQ(V.SourceHash, sha256(Original));
}

TEST(WebSourceView, DefaultProjectionHidesAllTargetNamesValuesAndComments) {
  const std::string Text = R"JS(#!/SECRET_INTERPRETER
// SECRET_LINE and sourceMappingURL=SECRET_MAP
/* SECRET_BLOCK */
const SECRET_NAME = "SECRET_VALUE";
const \u0053ECRET_ESCAPED = 123456789;
const SECRET_REGEX = /SECRET_PATTERN/giu;
const SECRET_TEMPLATE = `SECRET_HEAD${SECRET_NAME}SECRET_MIDDLE${`SECRET_NESTED${SECRET_NAME}`}SECRET_TAIL`;
const SECRET_OBJECT = {default: SECRET_NAME, null: true, false: 99n};
class SECRET_CLASS { #SECRET_PRIVATE = null; read() { return this.#SECRET_PRIVATE; } }
SECRET_OBJECT.default;
)JS";
  const auto V = view(Text);
  EXPECT_EQ(V.Text.find("SECRET"), std::string::npos);
  EXPECT_EQ(V.Text.find("123456789"), std::string::npos);
  EXPECT_EQ(V.Text.find("default"), std::string::npos);
  EXPECT_EQ(V.Text.find("true"), std::string::npos);
  EXPECT_NE(V.Text.find("const "), std::string::npos);
  EXPECT_NE(V.Text.find("[hashbang]"), std::string::npos);
  EXPECT_NE(V.Text.find("[template]"), std::string::npos);
  EXPECT_NE(V.Text.find("[regexp]"), std::string::npos);
  EXPECT_EQ(V.ReviewedBytes, 0);
  EXPECT_GT(V.HiddenRegions, 20);
  coverage(V, Text);
  EXPECT_EQ(V.ID, view(Text).ID);
}

TEST(WebSourceView, AliasesFollowLexicalIdentityAndKeepDynamicUsesUnresolved) {
  const std::string Text =
      "let secret = 1; secret; { let secret = 2; secret; } "
      "with (obj) { secret; }";
  const auto V = view(Text);
  std::vector<std::string> Names;
  std::vector<uint32_t> Bindings;
  for (const auto &S : V.Segments)
    if (Text.substr(S.SourceStart, S.SourceEnd - S.SourceStart) == "secret") {
      Names.push_back(V.Text.substr(S.Start, S.End - S.Start));
      Bindings.push_back(S.Binding);
    }
  ASSERT_EQ(Names.size(), 5);
  EXPECT_EQ(Names[0], Names[1]);
  EXPECT_EQ(Names[2], Names[3]);
  EXPECT_NE(Names[0], Names[2]);
  EXPECT_NE(Names[0], Names[4]);
  EXPECT_EQ(Bindings[4], NoSourceIndex);
  coverage(V, Text);
}

TEST(WebSourceView, UnsupportedBindingsCanStillProduceOccurrenceAliases) {
  const std::string Text = "const secret = 1; secret;";
  const auto S = source(Text);
  auto B = analyzeSourceBindings(S);
  B.Status = "budget_exceeded";
  // Even stale retained binding records cannot be used after failure.
  const auto V = makeSourceView(S, B, Text, sourceViewPolicy({}));
  EXPECT_EQ(V.BindingStatus, "budget_exceeded");
  EXPECT_EQ(V.Text.find("binding_"), std::string::npos);
  EXPECT_EQ(V.Text.find("secret"), std::string::npos);
  EXPECT_NE(V.Text.find("identifier_"), std::string::npos);
}

TEST(WebSourceView, ReviewDisclosesOnlyWholeExplicitRegionsAndChangesIdentity) {
  const std::string Text =
      "const secret = \"PRIVATE😀\";\r\n/* PRIVATE_COMMENT */";
  const auto Start = Text.find('"'), End = Text.find('"', Start + 1) + 1;
  const auto V = view(Text, review(Start, End - Start));
  EXPECT_NE(V.Text.find("PRIVATE😀"), std::string::npos);
  EXPECT_EQ(V.Text.find("PRIVATE_COMMENT"), std::string::npos);
  EXPECT_EQ(V.Text.find("secret"), std::string::npos);
  EXPECT_EQ(V.ReviewedBytes, End - Start);
  coverage(V, Text);
  EXPECT_NE(V.ID, view(Text).ID);
  const auto All = view(Text, review(0, Text.size()));
  EXPECT_EQ(All.Text, Text);
  EXPECT_EQ(All.ReviewedBytes, Text.size());
  EXPECT_EQ(All.HiddenRegions, 0);
  coverage(All, Text);
  EXPECT_THROW(view(Text, review(Start + 1, End - Start - 1)), Error);
  EXPECT_THROW(view(Text, review(Text.find("😀") + 1, 1)), Error);
  EXPECT_THROW(view(Text, review(Text.find('\n'), 1)), Error);
  EXPECT_THROW(view(Text, review(0, Text.size() + 1)), Error);
}

TEST(WebSourceView,
     StrictPoliciesRejectImplicitReviewAmbiguityAndForgedIdentity) {
  for (
      const auto Options :
      {R"({"schema_version":1,"reviewed_ranges":[{"byte_offset":"0","byte_length":"1"}]})",
       R"({"schema_version":1,"locally_reviewed":"yes"})",
       R"({"schema_version":1,"execute":true})",
       R"({"schema_version":1,"locally_reviewed":true,"reviewed_ranges":[{"byte_offset":0,"byte_length":"1"}]})",
       R"({"schema_version":1,"locally_reviewed":true,"reviewed_ranges":[{"byte_offset":"00","byte_length":"1"}]})",
       R"({"schema_version":1,"locally_reviewed":true,"reviewed_ranges":[{"byte_offset":"0","byte_length":"0"}]})",
       R"({"schema_version":1,"locally_reviewed":true,"reviewed_ranges":[{"byte_offset":"0","byte_length":"2"},{"byte_offset":"1","byte_length":"1"}]})",
       R"({"schema_version":1,"schema_version":1})"})
    EXPECT_THROW(sourceViewPolicy(Options), Error) << Options;
  const auto S = source("a;");
  auto Policy = sourceViewPolicy({});
  Policy.Reviewed.push_back({0, 2});
  EXPECT_THROW(makeSourceView(S, analyzeSourceBindings(S), "a;", Policy),
               Error);
  const auto Canonical = sourceViewPolicy(
      R"({"schema_version":1,"locally_reviewed":true,"reviewed_ranges":[{"byte_offset":"1","byte_length":"1"},{"byte_offset":"0","byte_length":"1"}]})");
  EXPECT_EQ(Canonical.Reviewed[0].Start, 0);
}

TEST(WebSourceView, EncodingGapsAndRegexRescansRemainCoveredAndPrivate) {
  for (const std::string Text :
       {"\xEF\xBB\xBF const secret = 1;\r\n",
        "const secret\xC2\xA0=\xE2\x80\xA8 1;",
        "if (true) /SECRET/.test('SECRET'); const q = a / b / c;",
        "const t = `SECRET${/SECRET/.test('SECRET') ? `SECRET` : 'SECRET'}`;",
        "", "/* SECRET */", "// SECRET\r\nx;", "#!/SECRET\r\nx;", " ",
        "\r\n"}) {
    const auto V = view(Text);
    EXPECT_EQ(V.Text.find("SECRET"), std::string::npos);
    EXPECT_EQ(V.Text.find("secret"), std::string::npos);
    coverage(V, Text);
  }
}

TEST(WebSourceView, UnavailableOrCorruptEvidenceFailsBeforePublication) {
  const std::string Text = "const a = 1;";
  auto S = source(Text);
  const auto B = analyzeSourceBindings(S);
  const auto P = sourceViewPolicy({});
  EXPECT_THROW(makeSourceView(S, B, "const a = 2;", P), Error);
  S.LexemeStatus = "source_lexeme_budget_exceeded";
  EXPECT_THROW(makeSourceView(S, B, Text, P), Error);
  S.LexemeStatus = "ok";
  S.Lexemes[0].End = Text.size() + 1;
  EXPECT_THROW(makeSourceView(S, B, Text, P), Error);
  auto Broken = source("const = ;");
  EXPECT_THROW(
      makeSourceView(Broken, analyzeSourceBindings(Broken), "const = ;", P),
      Error);
}

TEST(WebSourceView, RetainedLexemeBudgetIsSeparateFromSyntaxValidity) {
  std::string Text;
  Text.reserve(5 * MaxJavaScriptLexemes);
  for (uint64_t I = 0; I < MaxJavaScriptLexemes - 1; ++I)
    Text += "/*x*/";
  const auto S = source(Text);
  ASSERT_EQ(S.ParseStatus, "parsed");
  ASSERT_EQ(S.LexemeStatus, "ok");
  ASSERT_EQ(S.Lexemes.size(), MaxJavaScriptLexemes - 1);
  Text += "/*x*/";
  const auto Over = source(Text);
  EXPECT_EQ(Over.ParseStatus, "parsed");
  EXPECT_EQ(Over.LexemeStatus, "source_lexeme_budget_exceeded");
  EXPECT_TRUE(Over.Lexemes.empty());
  EXPECT_THROW(makeSourceView(Over, analyzeSourceBindings(Over), Text,
                              sourceViewPolicy({})),
               Error);
}

TEST(WebSourceView,
     LocationsDistinguishExactBoundariesFromHiddenRegionCoverage) {
  const std::string Text = "const secret = 'PRIVATE😀';\r\n";
  const auto V = view(Text);
  const auto Start = Text.find("PRIVATE");
  auto R = locateSourceView(V, Start, Start + 1);
  EXPECT_EQ(R.Mapping, "region_cover");
  EXPECT_EQ(V.Text.substr(R.Start, R.End - R.Start), "[string]");
  EXPECT_EQ(Text.substr(R.SourceStart, R.SourceEnd - R.SourceStart),
            "'PRIVATE😀'");
  R = locateSourceView(V, Start, Start);
  EXPECT_EQ(R.Mapping, "region_cover");
  EXPECT_EQ(V.Text.substr(R.Start, R.End - R.Start), "[string]");
  R = locateSourceView(V, Start - 1, Start - 1);
  EXPECT_EQ(R.Mapping, "exact_boundary");
  EXPECT_EQ(R.Start, R.End);
  R = locateSourceView(V, 1, 3);
  EXPECT_EQ(R.Mapping, "byte_identity");
  EXPECT_EQ(V.Text.substr(R.Start, R.End - R.Start), "on");
  R = locateSourceView(V, 0, Text.size());
  EXPECT_EQ(R.Start, 0);
  EXPECT_EQ(R.End, V.Text.size());
  EXPECT_EQ(R.LastSegment, V.Segments.size());
  R = locateSourceView(V, Text.size(), Text.size());
  EXPECT_EQ(R.Mapping, "exact_boundary");
  EXPECT_EQ(R.Start, V.Text.size());
  EXPECT_EQ(R.FirstSegment, V.Segments.size());
  EXPECT_THROW(locateSourceView(V, Text.size() + 1, Text.size() + 1), Error);
  EXPECT_THROW(locateSourceView(V, 2, 1), Error);
  const auto Clear = view(Text, review(0, Text.size()));
  R = locateSourceView(Clear, Start, Start + 7);
  EXPECT_EQ(R.Mapping, "byte_identity");
  EXPECT_EQ(R.Start, Start);
  EXPECT_EQ(R.End, Start + 7);
}
} // namespace
