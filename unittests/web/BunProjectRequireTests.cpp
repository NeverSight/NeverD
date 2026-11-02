//===- BunProjectRequireTests.cpp - Bound Bun require alias tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert, self-authored cross-module alias fixtures; no target is executed.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/BunProjectRequire.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"

#include <algorithm>
#include <type_traits>

using namespace neverd::web;

namespace {
constexpr std::string_view ProviderPath = "/$bunfs/root/provider.js";
constexpr std::string_view ProviderText =
    "var r=import.meta.require;export{r as load};";
constexpr std::string_view ConsumerText =
    "import{load as read}from'/$bunfs/root/provider.js';"
    "const value=read('/$bunfs/root/asset.node');";

std::vector<BunProjectSourceTarget> targets() {
  return {{std::string(ProviderPath), "m00007.js"},
          {"/$bunfs/root/asset.node", "m00008.node"}};
}
BunProjectRequireProviders provider(std::string_view Text = ProviderText) {
  return collectBunProjectRequireProviders("provider", Text, "module",
                                           ProviderPath);
}
bool diagnostic(const std::vector<SourceDiagnostic> &Diagnostics,
                std::string_view Code) {
  return std::any_of(Diagnostics.begin(), Diagnostics.end(),
                     [&](const auto &D) { return D.Code == Code; });
}
} // namespace

static_assert(!std::is_constructible_v<BunProjectRequireProvider,
                                       BunProjectRequireProvider::Data>);
static_assert(!std::is_constructible_v<BunProjectRequireCall,
                                       BunProjectRequireCall::Data>);
static_assert(std::is_same_v<decltype(BunProjectRequireCall{}.data()),
                             const BunProjectRequireCall::Data &>);

TEST(WebBunProjectRequire, LinksRenamedBindingsAndRetainsExactEvidence) {
  const auto P = provider();
  ASSERT_EQ(P.Status, "ok");
  ASSERT_EQ(P.Providers.size(), 1);
  const auto &Seed = P.Providers.front().data();
  EXPECT_EQ(Seed.VirtualName, u"/$bunfs/root/provider.js");
  EXPECT_EQ(Seed.ExportedName, u"load");
  EXPECT_EQ(Seed.SourceHash, sha256(ProviderText));
  EXPECT_EQ(ProviderText.substr(Seed.InitializerStart,
                                Seed.InitializerEnd - Seed.InitializerStart),
            "import.meta.require");
  const auto C = proveBunProjectRequireCalls("consumer", ConsumerText, "module",
                                             P.Providers, targets());
  ASSERT_EQ(C.Status, "ok");
  ASSERT_EQ(C.Calls.size(), 1);
  EXPECT_TRUE(C.Diagnostics.empty());
  EXPECT_TRUE(validBunProjectRequireCall(C.Calls[0]));
  const auto &Call = C.Calls[0].data();
  EXPECT_EQ(Call.SourceArtifactID, "consumer");
  EXPECT_EQ(Call.SourceHash, sha256(ConsumerText));
  EXPECT_EQ(Call.Target, u"/$bunfs/root/asset.node");
  EXPECT_EQ(Call.ProviderID, Seed.ID);
  EXPECT_EQ(Call.ProviderSourceHash, Seed.SourceHash);
  EXPECT_EQ(Call.ProviderBindingID, Seed.BindingID);
  EXPECT_EQ(Call.ProviderExportID, Seed.ExportID);
  EXPECT_FALSE(Call.ConsumerBindingID.empty());
  EXPECT_FALSE(Call.ConsumerImportID.empty());
  EXPECT_EQ(
      ConsumerText.substr(Call.CalleeStart, Call.CalleeEnd - Call.CalleeStart),
      "read");
  EXPECT_EQ(ConsumerText.substr(Call.OriginalStart,
                                Call.OriginalEnd - Call.OriginalStart),
            "'/$bunfs/root/asset.node'");
  EXPECT_EQ(ConsumerText.substr(Call.CallStart, Call.CallEnd - Call.CallStart),
            "read('/$bunfs/root/asset.node')");
  const auto Again = proveBunProjectRequireCalls(
      "consumer", ConsumerText, "module", P.Providers, targets());
  ASSERT_EQ(Again.Calls.size(), 1);
  EXPECT_EQ(Again.Calls[0].data().ID, Call.ID);
}

TEST(WebBunProjectRequire, ProofObjectsRequireTheirOwningFactory) {
  EXPECT_FALSE(validBunProjectRequireCall(BunProjectRequireCall{}));
  const std::vector<BunProjectRequireProvider> Invalid(1);
  const auto C = proveBunProjectRequireCalls("consumer", ConsumerText, "module",
                                             Invalid, targets());
  EXPECT_EQ(C.Status, "bun_require_invalid_provider_evidence");
  EXPECT_TRUE(C.Calls.empty());
  const auto P = provider();
  ASSERT_EQ(P.Providers.size(), 1);
  // Data may be copied for inspection, but changing it cannot mutate a proof.
  auto Copy = P.Providers[0].data();
  Copy.SourceHash = "changed";
  EXPECT_EQ(P.Providers[0].data().SourceHash, sha256(ProviderText));
}

TEST(WebBunProjectRequire, ExternalRequestsAreNotMissingVirtualTargets) {
  const auto P = provider();
  const std::string Text =
      "import{load as r}from'/$bunfs/root/provider.js';r('child_process');";
  const auto C = proveBunProjectRequireCalls("consumer", Text, "module",
                                             P.Providers, targets());
  ASSERT_EQ(C.Status, "ok");
  EXPECT_TRUE(C.Calls.empty());
  EXPECT_TRUE(C.Diagnostics.empty());
  ASSERT_EQ(C.ExternalReferences.size(), 1u);
  const auto &Reference = C.ExternalReferences[0];
  EXPECT_EQ(Reference.Target, u"child_process");
  EXPECT_EQ(Text.substr(Reference.Start, Reference.End - Reference.Start),
            "'child_process'");
}

TEST(WebBunProjectRequire, ExternalReferenceFailureClearsPartialInventory) {
  const auto P = provider();
  std::string Text = "import{load as r}from'/$bunfs/root/provider.js';";
  for (uint64_t I = 0; I <= MaxBunProjectRequireRecords; ++I)
    Text += "r('node:fs');";
  const auto C = proveBunProjectRequireCalls("consumer", Text, "module",
                                             P.Providers, targets());
  EXPECT_EQ(C.Status, "bun_require_record_budget_exceeded");
  EXPECT_TRUE(C.Calls.empty());
  EXPECT_TRUE(C.ExternalReferences.empty());
  ASSERT_EQ(C.Diagnostics.size(), 1u);
  EXPECT_EQ(C.Diagnostics[0].Code, "bun_require_record_budget_exceeded");
}

TEST(WebBunProjectRequire, ProviderWritesDuplicatesAndDynamicScopesRefuse) {
  for (const std::string Text : {
           "var r=import.meta.require;r=other;export{r as load};",
           "var r=import.meta.require;function f(){r=other}export{r as load};",
           "var r=import.meta.require;r++;export{r as load};",
           "var r=import.meta.require;var r;export{r as load};",
           "var r=import.meta.require;var r=other;export{r as load};",
           "var r=import.meta.require;eval('r=other');export{r as load};",
           "let r;{r=import.meta.require}export{r as load};",
           "var {r}=import.meta.require;export{r as load};",
       }) {
    SCOPED_TRACE(Text);
    const auto P = provider(Text);
    EXPECT_TRUE(P.Providers.empty());
    EXPECT_FALSE(P.Diagnostics.empty());
  }
}

TEST(WebBunProjectRequire,
     ProviderMetaMutationEscapeAndDependencyGraphsRefuse) {
  for (const std::string Text : {
           "import.meta.require=other;var r=import.meta.require;export{r as "
           "load};",
           "var m=import.meta;var r=import.meta.require;export{r as load};",
           "use(import.meta);var r=import.meta.require;export{r as load};",
           "Object.defineProperty(import.meta,'require',{});var "
           "r=import.meta.require;export{r as load};",
           "var r=import.meta['require'];export{r as load};",
           "var r=import.meta.require.bind(import.meta);export{r as load};",
           "var r=import.meta.require;import "
           "'/$bunfs/root/consumer.js';export{r as load};",
           "export{load}from'/$bunfs/root/provider.js';",
           "export*from'/$bunfs/root/provider.js';",
       }) {
    SCOPED_TRACE(Text);
    const auto P = provider(Text);
    EXPECT_TRUE(P.Providers.empty());
    EXPECT_NE(P.Status, "ok");
  }
}

TEST(WebBunProjectRequire, ConsumerBindingOwnershipDefeatsNameMatching) {
  const auto P = provider();
  ASSERT_EQ(P.Providers.size(), 1);
  for (const std::string Text : {
           "function read(x){return x}read('/$bunfs/root/asset.node');",
           "import{load as read}from'/$bunfs/root/provider.js';function "
           "f(read){read('/$bunfs/root/asset.node')}",
           "import{other as "
           "read}from'/$bunfs/root/provider.js';read('/$bunfs/root/"
           "asset.node');",
           "import{load as "
           "read}from'/$bunfs/root/other.js';read('/$bunfs/root/asset.node');",
           "import read "
           "from'/$bunfs/root/provider.js';read('/$bunfs/root/asset.node');",
           "import*as ns "
           "from'/$bunfs/root/provider.js';ns.load('/$bunfs/root/asset.node');",
           "import{load as read}from'/$bunfs/root/provider.js';const "
           "alias=read;alias('/$bunfs/root/asset.node');",
           "import{load as "
           "read}from'/$bunfs/root/provider.js';read=other;read('/$bunfs/root/"
           "asset.node');",
           "import{load as read}from'/$bunfs/root/provider.js';function "
           "f(){read=other}read('/$bunfs/root/asset.node');",
           "import{load as "
           "read}from'/$bunfs/root/provider.js';eval('read');read('/$bunfs/"
           "root/asset.node');",
       }) {
    SCOPED_TRACE(Text);
    const auto C = proveBunProjectRequireCalls("consumer", Text, "module",
                                               P.Providers, targets());
    EXPECT_TRUE(C.Calls.empty());
  }
}

TEST(WebBunProjectRequire, OnlySingleLiteralOrdinaryCallsAreCandidates) {
  const auto P = provider();
  ASSERT_EQ(P.Providers.size(), 1);
  const std::string Prefix =
      "import{load as read}from'/$bunfs/root/provider.js';";
  for (const std::string Call :
       {"read(path);", "read();", "read('/$bunfs/root/asset.node',other);",
        "read(...['/$bunfs/root/asset.node']);",
        "read(`/$bunfs/root/asset.node`);",
        "read?.('/$bunfs/root/asset.node');",
        "new read('/$bunfs/root/asset.node');",
        "read.call(null,'/$bunfs/root/asset.node');",
        "read('/$bunfs/root/missing.node');"}) {
    SCOPED_TRACE(Call);
    const auto C = proveBunProjectRequireCalls(
        "consumer", Prefix + Call, "module", P.Providers, targets());
    EXPECT_TRUE(C.Calls.empty());
  }
  const auto Unknown = proveBunProjectRequireCalls(
      "consumer", Prefix + "read('/$bunfs/root/missing.node');", "module",
      P.Providers, targets());
  EXPECT_TRUE(
      diagnostic(Unknown.Diagnostics, "bun_require_unknown_virtual_target"));
}

TEST(WebBunProjectRequire,
     DuplicateMissingAndChangedProviderEvidenceCannotJoin) {
  auto P = provider();
  ASSERT_EQ(P.Providers.size(), 1);
  P.Providers.push_back(P.Providers[0]);
  auto C = proveBunProjectRequireCalls("consumer", ConsumerText, "module",
                                       P.Providers, targets());
  EXPECT_EQ(C.Status, "bun_require_ambiguous_provider");
  EXPECT_TRUE(C.Calls.empty());
  P = provider();
  auto Targets = targets();
  Targets.erase(Targets.begin());
  C = proveBunProjectRequireCalls("consumer", ConsumerText, "module",
                                  P.Providers, Targets);
  EXPECT_TRUE(C.Calls.empty());
  EXPECT_TRUE(
      diagnostic(C.Diagnostics, "bun_require_provider_import_unverified"));
  const auto Changed = provider("var r=other;export{r as load};");
  EXPECT_TRUE(Changed.Providers.empty());
  C = proveBunProjectRequireCalls("consumer", ConsumerText, "module",
                                  Changed.Providers, targets());
  EXPECT_TRUE(C.Calls.empty());
}

TEST(WebBunProjectRequire, EscapedLiteralValuesPreserveOriginalSpans) {
  const auto P = provider();
  ASSERT_EQ(P.Providers.size(), 1);
  const std::string Text = "import{load as read}from'/$bunfs/root/provider.js';"
                           "read('/$bunfs/root/\\u0061sset.node');";
  const auto C = proveBunProjectRequireCalls("consumer", Text, "module",
                                             P.Providers, targets());
  ASSERT_EQ(C.Status, "ok");
  ASSERT_EQ(C.Calls.size(), 1);
  const auto &Call = C.Calls[0].data();
  EXPECT_EQ(Call.Target, u"/$bunfs/root/asset.node");
  EXPECT_EQ(
      Text.substr(Call.OriginalStart, Call.OriginalEnd - Call.OriginalStart),
      "'/$bunfs/root/\\u0061sset.node'");
}

TEST(WebBunProjectRequire,
     BudgetsParseFailureAndNonModuleSourcesPublishNoProof) {
  EXPECT_TRUE(provider("const =;").Providers.empty());
  auto P = collectBunProjectRequireProviders("provider", ProviderText, "script",
                                             ProviderPath);
  EXPECT_EQ(P.Status, "bun_require_module_source_required");
  EXPECT_TRUE(P.Providers.empty());
  P = provider();
  ASSERT_EQ(P.Providers.size(), 1);
  const auto Invalid = proveBunProjectRequireCalls(
      "consumer", "const =;", "module", P.Providers, targets());
  EXPECT_EQ(Invalid.Status, "bun_require_source_unavailable");
  EXPECT_TRUE(Invalid.Calls.empty());
  const std::vector<BunProjectRequireProvider> TooMany(
      MaxBunProjectRequireRecords + 1);
  const auto Over = proveBunProjectRequireCalls("consumer", ConsumerText,
                                                "module", TooMany, targets());
  EXPECT_EQ(Over.Status, "bun_require_record_budget_exceeded");
  EXPECT_TRUE(Over.Calls.empty());
  auto Duplicate = targets();
  Duplicate.push_back(Duplicate[0]);
  const auto Mapping = proveBunProjectRequireCalls(
      "consumer", ConsumerText, "module", P.Providers, Duplicate);
  EXPECT_EQ(Mapping.Status, "bun_require_invalid_target_mapping");
  EXPECT_TRUE(Mapping.Calls.empty());
}
