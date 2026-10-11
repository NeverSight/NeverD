//===- SourceRecoveryTests.cpp - Verified readable source recovery tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verified readable source recovery tests.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Bun.h"
#include "neverd/web/SourceRecovery.h"

#include <algorithm>
#include <cstdlib>

using namespace neverd::web;

TEST(WebSourceRecovery, KeepsCommentsLiteralsTemplatesAndASIWhileFormatting) {
  const std::string Input = R"JS(// license must remain
import{x}from'./dependency.js';function f(a){if(a){return /[{};]/g.test(`x${a};`)}return
{x:1}}const o={return(){return '};not syntax'}},v=()=>({a:1});for(let i=0;i<3;i++){f(i)}
)JS";
  const auto R = recoverReadableJavaScript("source", Input, "module");
  EXPECT_EQ(R.ParseStatus, "parsed");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_NE(R.Text.find("// license must remain"), std::string::npos);
  EXPECT_NE(R.Text.find("/[{};]/g"), std::string::npos);
  EXPECT_NE(R.Text.find("`x${a};`"), std::string::npos);
  EXPECT_GT(std::count(R.Text.begin(), R.Text.end(), '\n'),
            std::count(Input.begin(), Input.end(), '\n'));
  // All original bytes survive, in order, even original whitespace.
  size_t At = 0;
  for (char C : R.Text)
    if (At < Input.size() && Input[At] == C)
      ++At;
  EXPECT_EQ(At, Input.size());
}

TEST(WebSourceRecovery, RejectsInvalidAndOverBudgetInputWithoutFakeSource) {
  auto R = recoverReadableJavaScript("source", "const = ;", "module");
  EXPECT_EQ(R.Status, "invalid_syntax");
  EXPECT_TRUE(R.Text.empty());
  ASSERT_FALSE(R.Diagnostics.empty());
  EXPECT_EQ(R.Diagnostics.front().Code, "syntax_error");
  EXPECT_GE(R.Diagnostics.front().ByteOffset, 0);
  R = recoverReadableJavaScript(
      "source", std::string(32 * 1024 * 1024 + 1, ' '), "module");
  EXPECT_EQ(R.Status, "source_byte_budget_exceeded");
  EXPECT_TRUE(R.Text.empty());
}

TEST(WebSourceRecovery, AsyncRestFormattingRetainsTheSameSyntaxTree) {
  const std::string Source = "const f=async(...args)=>{try{return await "
                             "run(...args)}finally{done()}};";
  const auto R = recoverReadableJavaScript("source", Source, "module");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_TRUE(R.Diagnostics.empty());
  EXPECT_NE(R.Text.find("async(...args)"), std::string::npos);
}

TEST(WebSourceRecovery, ResourceDeclarationsRetainDisposalSyntaxAndSameTree) {
  const std::string Input =
      "async function f(){using /* keep */ x={[Symbol.dispose](){done()}};"
      "await using y={[Symbol.asyncDispose]:async()=>{await done()}};"
      "for(await using z of values){use(z)}return x}";
  const auto R = recoverReadableJavaScript("source", Input, "module");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_NE(R.Text.find("using /* keep */ x"), std::string::npos);
  EXPECT_NE(R.Text.find("await using y"), std::string::npos);
  EXPECT_NE(R.Text.find("await using z"), std::string::npos);
  size_t At = 0;
  for (char C : R.Text)
    if (At < Input.size() && Input[At] == C)
      ++At;
  EXPECT_EQ(At, Input.size());
}

TEST(WebSourceRecovery, LargeRecoveryDoesNotRaiseInteractiveParserLimits) {
  const std::string Source =
      "const x='" + std::string(2 * 1024 * 1024, 'a') + "';";
  EXPECT_ANY_THROW(inspectJavaScript("source", Source, "module"));
  const auto R = recoverReadableJavaScript("source", Source, "module");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_TRUE(R.Text.starts_with(Source));
}

TEST(WebSourceRecovery, ClaudeCode21296AllJavaScriptWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional official artifact not supplied; no target code "
                    "is downloaded or redistributed";
  const auto Snapshot = capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1);
  const auto &Original = Snapshot.Artifacts.front();
  ASSERT_EQ(Original.BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Original);
  uint64_t Sources = 0, Bytes = 0;
  for (const auto &M : E.Modules) {
    if (M.SourceArtifactID.empty())
      continue;
    SCOPED_TRACE(M.ID);
    const auto Source = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
    ASSERT_TRUE(M.Format == 1 || M.Format == 2);
    const auto Readable = recoverReadableJavaScript(
        M.SourceArtifactID, Source, M.Format == 1 ? "module" : "commonjs");
    for (const auto &D : Readable.Diagnostics)
      ADD_FAILURE() << "recovery diagnostic: " << D.Code << " at "
                    << D.ByteOffset;
    EXPECT_EQ(Readable.Status, "verified_same_parser_tree");
    EXPECT_EQ(Readable.ParseStatus, "parsed");
    EXPECT_TRUE(Readable.Diagnostics.empty());
    size_t At = 0;
    for (char C : Readable.Text)
      if (At < Source.size() && Source[At] == C)
        ++At;
    EXPECT_EQ(At, Source.size());
    ++Sources;
    Bytes += Source.size();
  }
  EXPECT_EQ(Sources, 2345);
  EXPECT_EQ(Bytes, 44768763);
}
