//===- BunProjectSourceTests.cpp - Bun source relocation tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bun source relocation tests. Fixtures are parsed, never executed.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/BunProjectRequire.h"
#include "../../lib/web/BunProjectSource.h"
#include "../../lib/web/RecoveryParser.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <set>

using namespace neverd::web;

namespace {
bool diagnostic(const BunProjectSource &Plan, std::string_view Code) {
  return std::any_of(Plan.Diagnostics.begin(), Plan.Diagnostics.end(),
                     [&](const auto &D) { return D.Code == Code; });
}
std::vector<BunProjectSourceTarget> targets() {
  return {{"/$bunfs/root/a.js", "m00001.js"},
          {"/$bunfs/root/b.js", "m00002.js"}};
}
} // namespace

TEST(WebBunProjectSource, RelocatesOnlyAdmittedLiteralReferenceKinds) {
  const std::string Input =
      R"JS(// /$bunfs/root/a.js must remain in this comment
import {x} from '/$bunfs/root/a.js';
export {y} from '/$bunfs/root/b.js';
export * from '/$bunfs/root/a.js';
const lazy=import('/$bunfs/root/b.js');
const native=import.meta.require('/$bunfs/root/a.js');
const untouched='/$bunfs/root/a.js';
export {x};
)JS";
  const auto R = planBunProjectSource("module", Input, "module", targets());
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  EXPECT_EQ(R.ParseStatus, "parsed");
  ASSERT_EQ(R.Edits.size(), 5);
  const std::set<std::string> ExpectedKinds{"import", "export_named",
                                            "export_all", "import_expression",
                                            "import_meta_require"};
  std::set<std::string> Kinds;
  uint64_t At = 0;
  for (const auto &Edit : R.Edits) {
    EXPECT_GE(Edit.Start, At);
    ASSERT_LT(Edit.Start, Edit.End);
    ASSERT_LE(Edit.End, Input.size());
    const auto Original = Input.substr(Edit.Start, Edit.End - Edit.Start);
    EXPECT_TRUE(Original == "'/$bunfs/root/a.js'" ||
                Original == "'/$bunfs/root/b.js'");
    EXPECT_TRUE(Edit.NewTarget == u"./m00001.js" ||
                Edit.NewTarget == u"./m00002.js");
    Kinds.insert(Edit.Kind);
    At = Edit.End;
  }
  EXPECT_EQ(Kinds, ExpectedKinds);
  EXPECT_NE(R.Text.find("// /$bunfs/root/a.js must remain"), std::string::npos);
  EXPECT_NE(R.Text.find("const untouched='/$bunfs/root/a.js'"),
            std::string::npos);
  ASSERT_EQ(R.UnresolvedRuntimeReferences.size(), 1);
  EXPECT_EQ(R.UnresolvedRuntimeReferences[0].Target, u"/$bunfs/root/a.js");
  EXPECT_EQ(Input.substr(R.UnresolvedRuntimeReferences[0].Start,
                         R.UnresolvedRuntimeReferences[0].End -
                             R.UnresolvedRuntimeReferences[0].Start),
            "'/$bunfs/root/a.js'");
}

TEST(WebBunProjectSource,
     DoesNotGuessRuntimeAliasesOrComputedAndOptionalCalls) {
  const std::string Input = R"JS(const path='/$bunfs/root/a.js';
const object={'/$bunfs/root/a.js':true};
function f(require){return require('/$bunfs/root/a.js')}
object.require('/$bunfs/root/a.js');
import.meta['require']('/$bunfs/root/a.js');
import.meta.require?.('/$bunfs/root/a.js');
const alias=import.meta.require;alias('/$bunfs/root/a.js');
)JS";
  const auto R = planBunProjectSource("module", Input, "module", targets());
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  EXPECT_TRUE(R.Edits.empty());
  EXPECT_EQ(R.Text, Input);
  EXPECT_EQ(R.UnresolvedRuntimeReferences.size(), 7);
  EXPECT_TRUE(diagnostic(R, "bun_project_unresolved_runtime_reference"));
}

TEST(WebBunProjectSource,
     ResolvesEscapedUTF16ValuesWithoutChangingOtherLiterals) {
  const std::string Input =
      R"JS(import '/$bunfs/root/\u00e9-\uD83D\uDE00.js';const lone='\uD800';const nul='\0';)JS";
  const std::vector<BunProjectSourceTarget> Targets{
      {"/$bunfs/root/\xc3\xa9-\xf0\x9f\x98\x80.js", "m00003.js"}};
  const auto R = planBunProjectSource("module", Input, "module", Targets);
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  ASSERT_EQ(R.Edits.size(), 1);
  EXPECT_EQ(R.Edits.front().OldTarget, u"/$bunfs/root/\u00e9-\U0001f600.js");
  EXPECT_EQ(R.Edits.front().NewTarget, u"./m00003.js");
  EXPECT_EQ(R.Text,
            R"JS(import "./m00003.js";const lone='\uD800';const nul='\0';)JS");
  EXPECT_TRUE(R.Diagnostics.empty());
}

TEST(WebBunProjectSource, PreservesUnknownLoneSurrogateAsUTF16Evidence) {
  const std::string Input = R"JS(import('/$bunfs/root/\uD800.js');)JS";
  const auto R = planBunProjectSource("module", Input, "module", targets());
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  EXPECT_TRUE(R.Edits.empty());
  EXPECT_EQ(R.Text, Input);
  ASSERT_EQ(R.UnresolvedRuntimeReferences.size(), 1);
  auto Expected = std::u16string(u"/$bunfs/root/");
  Expected.push_back(char16_t(0xd800));
  Expected += u".js";
  EXPECT_EQ(R.UnresolvedRuntimeReferences.front().Target, Expected);
  EXPECT_TRUE(diagnostic(R, "bun_project_unknown_virtual_target"));
}

TEST(WebBunProjectSource,
     ReportsUnknownNonliteralAndUnsupportedArityExplicitly) {
  const std::string Input = R"JS(import '/$bunfs/root/a.js';
import('/$bunfs/root/missing.js');
import(path);import(`/$bunfs/root/a.js`);
import.meta.require(path);
import.meta.require('/$bunfs/root/a.js','/$bunfs/root/b.js');
)JS";
  const auto R = planBunProjectSource("module", Input, "module", targets());
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  ASSERT_EQ(R.Edits.size(), 1);
  EXPECT_EQ(R.UnresolvedRuntimeReferences.size(), 3);
  EXPECT_TRUE(diagnostic(R, "bun_project_unknown_virtual_target"));
  EXPECT_TRUE(diagnostic(R, "bun_project_nonliteral_module_reference"));
  EXPECT_TRUE(diagnostic(R, "bun_project_import_meta_require_arity"));
  EXPECT_NE(R.Text.find("import.meta.require('/$bunfs/root/a.js',"),
            std::string::npos);
  EXPECT_NE(R.Text.find("import(`/$bunfs/root/a.js`)"), std::string::npos);
}

TEST(WebBunProjectSource, RejectsAmbiguousMappingBeforePublishingAnyCandidate) {
  const std::string Input = "import '/$bunfs/root/a.js';";
  auto Targets = targets();
  Targets[1].VirtualName = Targets[0].VirtualName;
  auto R = planBunProjectSource("module", Input, "module", Targets);
  EXPECT_EQ(R.Status, "bun_project_duplicate_virtual_name");
  EXPECT_TRUE(R.Text.empty());
  EXPECT_TRUE(R.Edits.empty());
  Targets = targets();
  Targets[1].OutputFile = "M00001.js";
  R = planBunProjectSource("module", Input, "module", Targets);
  EXPECT_EQ(R.Status, "bun_project_duplicate_output_file");
  EXPECT_TRUE(R.Text.empty());
  EXPECT_TRUE(R.Edits.empty());
}

TEST(WebBunProjectSource, RejectsUnsafeOutputAndInvalidVirtualIdentities) {
  const std::string Input = "import '/$bunfs/root/a.js';";
  for (const std::string Name :
       {"../m00001.js", "dir/m00001.js", "CON.js", "m00001.js.", "m00001.js ",
        "\xc3\xa9.js", "m00001#x.js", "m00001%2f.js"}) {
    SCOPED_TRACE(Name);
    auto Targets = targets();
    Targets[0].OutputFile = Name;
    const auto R = planBunProjectSource("module", Input, "module", Targets);
    EXPECT_EQ(R.Status, "bun_project_invalid_output_file");
    EXPECT_TRUE(R.Text.empty());
    EXPECT_TRUE(R.Edits.empty());
  }
  for (const auto &Name :
       {std::string("/ordinary/a.js"), std::string("/$bunfs/root/\xc0\xaf.js"),
        std::string("/$bunfs/root/\0a.js", 19)}) {
    auto Targets = targets();
    Targets[0].VirtualName = Name;
    const auto R = planBunProjectSource("module", Input, "module", Targets);
    EXPECT_EQ(R.Status, "bun_project_invalid_virtual_name");
    EXPECT_TRUE(R.Text.empty());
  }
}

TEST(WebBunProjectSource, PreservesExternalImportsAndWindowsIdentityExactly) {
  const std::string Input =
      "import 'node:fs';import './relative.js';"
      "import('B:/~BUN/root/a.js');import('b:/~BUN/root/a.js');";
  const std::vector<BunProjectSourceTarget> Targets{
      {"B:/~BUN/root/a.js", "m00001.js"}};
  const auto R = planBunProjectSource("module", Input, "module", Targets);
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  ASSERT_EQ(R.Edits.size(), 1);
  EXPECT_NE(R.Text.find("import 'node:fs';import './relative.js';"),
            std::string::npos);
  EXPECT_NE(R.Text.find("import('b:/~BUN/root/a.js')"), std::string::npos);
  EXPECT_TRUE(R.Diagnostics.empty());
  ASSERT_EQ(R.ExternalModuleReferences.size(), 3);
  EXPECT_EQ(R.ExternalModuleReferences[0].Target, u"node:fs");
  EXPECT_EQ(R.ExternalModuleReferences[1].Target, u"./relative.js");
  EXPECT_EQ(R.ExternalModuleReferences[2].Target, u"b:/~BUN/root/a.js");
}

TEST(WebBunProjectSource,
     IsDeterministicAcrossMappingOrderAndKeepsSourceGoals) {
  const std::string Input =
      "function load(){return import('/$bunfs/root/b.js')}";
  auto Targets = targets();
  const auto A = planBunProjectSource("module", Input, "commonjs", Targets);
  std::reverse(Targets.begin(), Targets.end());
  const auto B = planBunProjectSource("module", Input, "commonjs", Targets);
  ASSERT_EQ(A.Status, "verified_literal_relocations");
  EXPECT_EQ(A.Text, B.Text);
  ASSERT_EQ(A.Edits.size(), 1);
  ASSERT_EQ(B.Edits.size(), 1);
  EXPECT_EQ(A.Edits.front().Start, B.Edits.front().Start);
  EXPECT_EQ(A.Edits.front().OldTarget, B.Edits.front().OldTarget);
  const auto InvalidGoal = planBunProjectSource(
      "module", "import '/$bunfs/root/a.js';", "script", Targets);
  EXPECT_NE(InvalidGoal.Status, "verified_literal_relocations");
  EXPECT_TRUE(InvalidGoal.Text.empty());
}

TEST(WebBunProjectSource, RejectsParseAndBudgetFailuresWithoutPartialSource) {
  auto R = planBunProjectSource("module", "const = ;", "module", targets());
  EXPECT_EQ(R.Status, "invalid_syntax");
  EXPECT_TRUE(R.Text.empty());
  EXPECT_TRUE(R.Edits.empty());
  EXPECT_TRUE(diagnostic(R, "syntax_error"));
  R = planBunProjectSource(
      "module", std::string(MaxRecoverySourceBytes + 1, ' '), "module", {});
  EXPECT_EQ(R.Status, "source_byte_budget_exceeded");
  EXPECT_TRUE(R.Text.empty());
  const std::vector<BunProjectSourceTarget> TooMany(MaxBunProjectSourceTargets +
                                                    1);
  R = planBunProjectSource("module", "", "module", TooMany);
  EXPECT_EQ(R.Status, "bun_project_target_budget_exceeded");
  EXPECT_TRUE(R.Text.empty());
  R = planBunProjectSource("module", "", "module", {});
  EXPECT_EQ(R.Status, "verified_literal_relocations");
  EXPECT_TRUE(R.Text.empty());
  EXPECT_TRUE(R.Edits.empty());
}

TEST(WebBunProjectSource, RelocatesOnlyCurrentImmutableImportedRequireProofs) {
  const std::string Provider = "var r=import.meta.require;export{r as load};";
  const std::string Consumer =
      "import {load as read} from '/$bunfs/root/provider.js';"
      "const x=read('/$bunfs/root/asset.txt');";
  const std::vector<BunProjectSourceTarget> Targets{
      {"/$bunfs/root/provider.js", "m00001.js"},
      {"/$bunfs/root/asset.txt", "m00002.txt"}};
  const auto Providers = collectBunProjectRequireProviders(
      "provider", Provider, "module", Targets[0].VirtualName);
  ASSERT_EQ(Providers.Providers.size(), 1);
  const auto Calls = proveBunProjectRequireCalls("consumer", Consumer, "module",
                                                 Providers.Providers, Targets);
  ASSERT_EQ(Calls.Calls.size(), 1);
  EXPECT_TRUE(validBunProjectRequireCall(Calls.Calls.front()));
  const auto R = planBunProjectSource("consumer", Consumer, "module", Targets,
                                      Calls.Calls);
  ASSERT_EQ(R.Status, "verified_literal_relocations");
  ASSERT_EQ(R.Edits.size(), 2);
  EXPECT_EQ(R.Edits.back().Kind, "imported_import_meta_require");
  EXPECT_EQ(R.Edits.back().OldTarget, u"/$bunfs/root/asset.txt");
  EXPECT_EQ(R.Edits.back().NewTarget, u"./m00002.txt");
  EXPECT_TRUE(R.UnresolvedRuntimeReferences.empty());
  EXPECT_NE(R.Text.find("read(\"./m00002.txt\")"), std::string::npos);

  const auto NoProof =
      planBunProjectSource("consumer", Consumer, "module", Targets);
  ASSERT_EQ(NoProof.Status, "verified_literal_relocations");
  EXPECT_EQ(NoProof.Edits.size(), 1);
  EXPECT_EQ(NoProof.UnresolvedRuntimeReferences.size(), 1);

  auto Invalid = planBunProjectSource("consumer", Consumer + " ", "module",
                                      Targets, Calls.Calls);
  EXPECT_EQ(Invalid.Status, "bun_project_invalid_require_proof");
  EXPECT_TRUE(Invalid.Text.empty());
  EXPECT_TRUE(Invalid.Edits.empty());
  Invalid = planBunProjectSource("different-artifact", Consumer, "module",
                                 Targets, Calls.Calls);
  EXPECT_EQ(Invalid.Status, "bun_project_invalid_require_proof");
  EXPECT_TRUE(Invalid.Text.empty());
  const std::vector<BunProjectRequireCall> Duplicated{Calls.Calls.front(),
                                                      Calls.Calls.front()};
  Invalid =
      planBunProjectSource("consumer", Consumer, "module", Targets, Duplicated);
  EXPECT_EQ(Invalid.Status, "bun_project_invalid_require_proof");
  EXPECT_TRUE(Invalid.Text.empty());
  const std::vector<BunProjectRequireCall> Default(1);
  EXPECT_FALSE(validBunProjectRequireCall(Default.front()));
  Invalid =
      planBunProjectSource("consumer", Consumer, "module", Targets, Default);
  EXPECT_EQ(Invalid.Status, "bun_project_invalid_require_proof");
  EXPECT_TRUE(Invalid.Text.empty());
  EXPECT_TRUE(Invalid.Edits.empty());
}
