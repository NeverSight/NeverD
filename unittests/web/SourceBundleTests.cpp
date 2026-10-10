#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceBundles.h"

#include "llvm/Support/JSON.h"

#include <fstream>

namespace {
using namespace neverd::web;
constexpr auto None = NoSourceIndex;

std::string fixture(std::string_view Name = "webpack-commonjs.bundle.txt") {
  std::ifstream File(std::string(NEVERD_WEB_FIXTURE_DIR) + "/" +
                         std::string(Name),
                     std::ios::binary);
  return {std::istreambuf_iterator<char>(File), {}};
}

// Never executed. Controlled C++ fixtures vary a fixed, in-process structural
// profile; the separately hashed upstream fixture supplies independent input.
std::string bundle(std::string_view Table,
                   std::string_view Startup = "load(0);") {
  return "(() => { var table = " + std::string(Table) + R"JS(;
    var cache = {};
    function load(id) {
      var cached = cache[id];
      if (cached !== undefined) { return cached.exports; }
      var module = cache[id] = { exports: {} };
      table[id](module, module.exports, load);
      return module.exports;
    }
  )JS" + std::string(Startup) +
         "})();";
}

struct Recovered {
  std::string Text;
  SourceAnalysis Source;
  SourceBindingAnalysis Bindings;
  SourceBundleAnalysis Bundles;
  explicit Recovered(std::string Text)
      : Text(std::move(Text)),
        Source(inspectJavaScript("bundle-fixture", this->Text, "script")),
        Bindings(analyzeSourceBindings(Source)),
        Bundles(analyzeSourceBundles(Source, Bindings, this->Text)) {}
};

TEST(WebBundles,
     PinnedUpstreamExampleRecoversSparseFactoriesAndStartupSeparately) {
  const auto Text = fixture();
  ASSERT_EQ(sha256(Text),
            "44ce144e4028a9c3ddbd3f619db4f337b687d1d716513fb51748adb1a767a1d5");
  const auto Document = fixture("webpack-commonjs-upstream.md");
  const auto Manifest = fixture("webpack-manifest.json");
  auto Parsed = llvm::json::parse(Manifest);
  ASSERT_TRUE(bool(Parsed));
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  EXPECT_EQ(Parsed->getAsObject()->getString("bundle_sha256"), sha256(Text));
  EXPECT_EQ(Parsed->getAsObject()->getString("source_sha256"),
            sha256(Document));
  std::string Reconstructed;
  size_t Position = Document.find("# dist/output.js");
  for (unsigned I = 0; I < 3; ++I) {
    const auto Fence = Document.find("```", Position);
    ASSERT_NE(Fence, std::string::npos);
    const auto Start = Document.find('\n', Fence) + 1;
    const auto End = Document.find("```", Start);
    ASSERT_NE(End, std::string::npos);
    Reconstructed.append(Document, Start, End - Start);
    Position = End + 3;
  }
  EXPECT_EQ(Reconstructed, Text);
  const Recovered R(Text), Again(Text);
  ASSERT_EQ(R.Source.ParseStatus, "parsed");
  ASSERT_EQ(R.Bundles.Status, "ok");
  ASSERT_EQ(R.Bundles.Bundles.size(), 1);
  ASSERT_EQ(R.Bundles.Modules.size(), 2);
  EXPECT_EQ(R.Bundles.Bundles[0].TableKind, "array");
  EXPECT_EQ(R.Bundles.Keys[R.Bundles.Modules[0].Key], u"1");
  EXPECT_EQ(R.Bundles.Keys[R.Bundles.Modules[1].Key], u"2");
  ASSERT_EQ(R.Bundles.Regions.size(), 2);
  ASSERT_EQ(R.Bundles.Dependencies.size(), 2);
  EXPECT_EQ(R.Bundles.Dependencies[0].CallerModule, 0);
  EXPECT_EQ(R.Bundles.Dependencies[0].TargetModule, 1);
  EXPECT_EQ(R.Bundles.Dependencies[1].CallerModule, None);
  EXPECT_EQ(R.Bundles.Dependencies[1].TargetModule, 0);
  for (size_t I = 0; I < R.Bundles.Modules.size(); ++I) {
    const auto &M = R.Bundles.Modules[I];
    const auto &N = R.Source.Nodes[M.Node], &Body = R.Source.Nodes[M.Body];
    EXPECT_EQ(M.ID, Again.Bundles.Modules[I].ID);
    EXPECT_EQ(M.WrapperHash, sha256(Text.substr(N.Start, N.End - N.Start)));
    EXPECT_EQ(M.BodyHash,
              sha256(Text.substr(Body.Start, Body.End - Body.Start)));
    EXPECT_LT(N.Start, Body.Start);
    EXPECT_EQ(Body.End, N.End);
  }
}

TEST(WebBundles, ObjectKeysAndRemovedSlotsRetainExactIdentityWithoutPaths) {
  const Recovered R(bundle(R"JS({
    './SECRET.js': (m,e,r) => { r('next'); },
    next: function(m,e) { e.value = 1; },
    7: false,
    '\uD800': () => {}
  })JS",
                           "load('./SECRET.js'); load(7); load('absent');"));
  ASSERT_EQ(R.Bundles.Status, "ok");
  ASSERT_EQ(R.Bundles.Modules.size(), 4);
  EXPECT_EQ(R.Bundles.Bundles[0].TableKind, "object");
  EXPECT_EQ(R.Bundles.Keys[R.Bundles.Modules[0].Key], u"./SECRET.js");
  EXPECT_EQ(R.Bundles.Keys[R.Bundles.Modules[3].Key],
            std::u16string(1, 0xd800));
  EXPECT_EQ(R.Bundles.Modules[2].Kind, "removed_slot");
  EXPECT_EQ(R.Bundles.Modules[2].Body, None);
  ASSERT_EQ(R.Bundles.Dependencies.size(), 4);
  EXPECT_EQ(R.Bundles.Dependencies[0].TargetModule, 1);
  EXPECT_EQ(R.Bundles.Dependencies[2].Status, "removed_target");
  EXPECT_EQ(R.Bundles.Dependencies[3].Status, "target_not_in_table");
}

TEST(WebBundles, AlphaMatchingRequiresLexicalIdentityAndExactDispatcherAbi) {
  const auto Text = bundle("[() => {}]");
  ASSERT_EQ(Recovered(Text).Bundles.Status, "ok");
  auto Replace = [&](std::string_view Before, std::string_view After) {
    auto Changed = Text;
    const auto Pos = Changed.find(Before);
    EXPECT_NE(Pos, std::string::npos);
    Changed.replace(Pos, Before.size(), After);
    return Recovered(Changed).Bundles;
  };
  for (const auto &R :
       {Replace("table[id](module, module.exports, load)",
                "table[id](module.exports, module, load)"),
        Replace("var module = cache[id]", "var module = other[id]"),
        Replace("return module.exports;", "return module.other;"),
        Replace("cached !== undefined", "cached === undefined"),
        Replace("function load(id)", "function load(id, extra)")}) {
    EXPECT_EQ(R.Status, "unsupported");
    EXPECT_TRUE(R.Modules.empty());
    ASSERT_FALSE(R.Diagnostics.empty());
    EXPECT_EQ(R.Diagnostics[0].Code, "unsupported_bundle_loader_layout");
  }
  const Recovered Shadow("var undefined = 1; " + Text);
  EXPECT_EQ(Shadow.Bundles.Status, "unsupported");
  const Recovered Plain(
      "const table = [() => {}]; function load(x) { return table[x](); }");
  EXPECT_EQ(Plain.Bundles.Status, "not_detected");
  EXPECT_TRUE(Plain.Bundles.Modules.empty());
}

TEST(WebBundles, TableMutationsFactoryParametersAndPrototypeKeysAreNotGuessed) {
  for (const auto Table :
       {"{1: () => {}, '1': () => {}}", "{'__proto__': () => {}}",
        "{[compute()]: () => {}}", "{ get key() { return () => {}; } }",
        "{...other}", "[...other]", "[async () => {}]", "[function*() {}]",
        "[(module,exports,require = other) => {}]", "[function(a,a,a) {}]",
        "[() => 42]", "[unknown]"}) {
    const Recovered R(bundle(Table));
    ASSERT_EQ(R.Source.ParseStatus, "parsed") << Table;
    EXPECT_EQ(R.Bundles.Status, "unsupported") << Table;
    EXPECT_TRUE(R.Bundles.Bundles.empty()) << Table;
    EXPECT_TRUE(R.Bundles.Modules.empty()) << Table;
  }
}

TEST(WebBundles, CallsRetainShadowingWritesDynamicAndOptionalUncertainty) {
  const Recovered R(bundle(R"JS([
    (m,e,r) => { r(1); function inner(r) { r(1); } r(dynamic); r?.(1); },
    (m,e,r) => { r = replacement; r(0); }
  ])JS",
                           "load(0);"));
  ASSERT_EQ(R.Bundles.Status, "ok");
  ASSERT_EQ(R.Bundles.Dependencies.size(), 5);
  EXPECT_EQ(R.Bundles.Dependencies[0].TargetModule, 1);
  EXPECT_EQ(R.Bundles.Dependencies[1].Status,
            "dynamic_or_unsupported_argument");
  EXPECT_EQ(R.Bundles.Dependencies[2].Status, "optional_call_boundary");
  EXPECT_EQ(R.Bundles.Dependencies[3].Status, "binding_written");
  EXPECT_EQ(R.Bundles.Dependencies[3].TargetModule, None);
  EXPECT_EQ(R.Bundles.Dependencies[4].TargetModule, 0);
  const Recovered TableChanged(bundle("[() => {}]", "table = other; load(0);"));
  ASSERT_EQ(TableChanged.Bundles.Status, "ok");
  EXPECT_TRUE(TableChanged.Bundles.Bundles[0].TableBindingWritten);
  ASSERT_EQ(TableChanged.Bundles.Dependencies.size(), 1);
  EXPECT_EQ(TableChanged.Bundles.Dependencies[0].Status, "binding_written");
  const Recovered Dynamic(bundle("[(m,e,r) => { with (object) { r(0); } }]"));
  ASSERT_EQ(Dynamic.Bundles.Status, "partial");
  EXPECT_EQ(Dynamic.Bundles.BindingStatus, "partial");
  ASSERT_EQ(Dynamic.Bundles.Dependencies.size(), 2);
  EXPECT_EQ(Dynamic.Bundles.Dependencies[0].Status, "dynamic_lookup");
  EXPECT_EQ(Dynamic.Bundles.Dependencies[0].CalleeBinding, None);
  EXPECT_EQ(Dynamic.Bundles.Dependencies[0].TargetModule, None);
  const Recovered LoadChanged(
      bundle("[(m,e,r) => { r(0); }]", "load = other; load(0);"));
  ASSERT_EQ(LoadChanged.Bundles.Status, "ok");
  ASSERT_EQ(LoadChanged.Bundles.Dependencies.size(), 2);
  EXPECT_EQ(LoadChanged.Bundles.Dependencies[0].Status, "binding_written");
}

TEST(WebBundles,
     MultipleBundlesKeepOccurrencesAndRejectOnlyTheMalformedLayout) {
  const auto One = bundle("[() => {}]");
  const Recovered R(One + One + bundle("{1: () => {}, '1': () => {}}"));
  ASSERT_EQ(R.Bundles.Status, "partial");
  ASSERT_EQ(R.Bundles.Bundles.size(), 2);
  ASSERT_EQ(R.Bundles.Modules.size(), 2);
  EXPECT_NE(R.Bundles.Modules[0].ID, R.Bundles.Modules[1].ID);
  EXPECT_EQ(R.Bundles.Modules[0].WrapperHash, R.Bundles.Modules[1].WrapperHash);
  ASSERT_EQ(R.Bundles.Dependencies.size(), 2);
  EXPECT_EQ(R.Bundles.Dependencies[0].TargetModule, 0);
  EXPECT_EQ(R.Bundles.Dependencies[1].TargetModule, 1);
}

TEST(WebBundles, GlobalLimitsDiscardEveryRecoveredPartition) {
  std::string Many;
  for (uint64_t I = 0; I <= MaxJavaScriptBundles; ++I)
    Many += bundle("[() => {}]");
  const Recovered R(Many);
  ASSERT_EQ(R.Source.ParseStatus, "parsed");
  EXPECT_EQ(R.Bundles.Status, "budget_exceeded");
  EXPECT_TRUE(R.Bundles.Bundles.empty());
  EXPECT_TRUE(R.Bundles.Modules.empty());
  EXPECT_TRUE(R.Bundles.Dependencies.empty());
  EXPECT_TRUE(R.Bundles.Keys.empty());
  std::string Table = "[";
  for (uint64_t I = 0; I <= MaxJavaScriptBundleModules; ++I)
    Table += I ? ",()=>{}" : "()=>{}";
  const Recovered Big(bundle(Table + "]"));
  ASSERT_EQ(Big.Source.ParseStatus, "parsed");
  EXPECT_EQ(Big.Bundles.Status, "budget_exceeded");
  EXPECT_TRUE(Big.Bundles.Modules.empty());
  ASSERT_FALSE(Big.Bundles.Diagnostics.empty());
  EXPECT_EQ(Big.Bundles.Diagnostics[0].Code,
            "source_bundle_module_budget_exceeded");
}

TEST(WebBundles, ImmutableSourceAndAnalysisIdentitiesAreRequired) {
  const auto Text = bundle("[() => {}]");
  Recovered R(Text);
  EXPECT_THROW(analyzeSourceBundles(R.Source, R.Bindings, Text + " "), Error);
  auto B = R.Bindings;
  B.SourceID = "other";
  EXPECT_THROW(analyzeSourceBundles(R.Source, B, Text), Error);
  B = R.Bindings;
  B.Status = "unsupported";
  EXPECT_EQ(analyzeSourceBundles(R.Source, B, Text).Status, "unavailable");
  auto S = R.Source;
  S.Nodes[0].Children[0].Index = S.Nodes.size();
  const auto Broken = analyzeSourceBundles(S, R.Bindings, Text);
  EXPECT_EQ(Broken.Status, "unsupported");
  EXPECT_TRUE(Broken.Modules.empty());
}
} // namespace
