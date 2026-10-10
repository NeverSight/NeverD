#include "gtest/gtest.h"

#include "neverd/web/ImportMap.h"
#include "neverd/web/Session.h"

namespace {
using namespace neverd::web;
constexpr std::string_view Base = "https://example.test/app/index.html";
ImportMapResolution resolve(const ImportMap &Map, std::string_view Name,
                            std::string_view ScriptBase = Base) {
  uint64_t Steps = 0;
  const ImportMap *Maps[]{&Map};
  return resolveImportMaps(Maps, Name, ScriptBase, [&](uint64_t N) {
    ASSERT_LE(N, MaxImportMapSteps - Steps);
    Steps += N;
  });
}
TEST(WebImportMap, NativeURLNormalizationRetainsIdentityComponents) {
  const auto Charge = [](uint64_t) {};
  EXPECT_EQ(normalizeModuleURL("HTTP://EXAMPLE.test:80/a/../b?x#y", {}, Charge),
            "http://example.test/b?x#y");
  EXPECT_EQ(normalizeModuleURL("./中.js?x#y", Base, Charge),
            "https://example.test/app/%E4%B8%AD.js?x#y");
  EXPECT_EQ(normalizeModuleURL("./%64ep.js?x", Base, Charge),
            "https://example.test/app/%64ep.js?x");
  EXPECT_EQ(normalizeModuleURL("./a/%2e%2e/dep.js", Base, Charge),
            "https://example.test/app/dep.js");
  EXPECT_NE(normalizeModuleURL("./dep.js?x", Base, Charge),
            normalizeModuleURL("./dep.js?y", Base, Charge));
  EXPECT_EQ(normalizeModuleURL("https://bücher.example/", {}, Charge),
            "https://xn--bcher-kva.example/");
  EXPECT_TRUE(normalizeModuleURL("http://[", {}, Charge).empty());
  EXPECT_TRUE(normalizeModuleURL("./\xff.js", Base, Charge).empty());
}
TEST(WebImportMap, ExactPrefixAndScopePrecedenceUseTheDeclarationBase) {
  const auto M = inspectImportMap("declaration", R"({
    "imports":{"pkg/":"../vendor/","pkg/exact":"./exact.js",
               "./dep.js":"./remapped.js","free":"./free.js"},
    "scopes":{"./":{"pkg/":"./scoped/"},
              "./nested/":{"free":"../nested.js"}}
  })",
                                  Base);
  ASSERT_EQ(M.Status, "ok") << M.Reason;
  EXPECT_EQ(resolve(M, "pkg/exact").URL,
            "https://example.test/app/scoped/exact");
  const auto Outside =
      resolve(M, "pkg/exact", "https://example.test/elsewhere/x");
  EXPECT_EQ(Outside.URL, "https://example.test/app/exact.js");
  EXPECT_EQ(Outside.MatchKind, "exact");
  EXPECT_TRUE(Outside.ScopeID.empty());
  const auto Nested =
      resolve(M, "pkg/a.js", "https://example.test/app/nested/x");
  EXPECT_EQ(Nested.URL, "https://example.test/app/scoped/a.js");
  EXPECT_FALSE(Nested.ScopeID.empty());
  EXPECT_EQ(resolve(M, "free", "https://example.test/app/nested/x").URL,
            "https://example.test/nested.js");
  EXPECT_EQ(resolve(M, "./dep.js").URL, "https://example.test/app/remapped.js");
  EXPECT_EQ(resolve(M, "./dep.js", "https://example.test/elsewhere/x").URL,
            "https://example.test/elsewhere/dep.js");
}
TEST(WebImportMap, NullAndInvalidAddressesBlockWithoutFallback) {
  const auto M = inspectImportMap("map", R"({
    "imports":{"blocked":"./fallback.js","p/":"./fallback/",
               "number":4,"array":[],"bare":"dep.js","bad/":"./file.js"},
    "scopes":{"./":{"blocked":null,"p/":false}}
  })",
                                  Base);
  ASSERT_EQ(M.Status, "ok") << M.Reason;
  for (const auto Name :
       {"blocked", "p/a.js", "number", "array", "bare", "bad/x"}) {
    const auto R = resolve(M, Name);
    EXPECT_EQ(R.Status, "blocked_import_map") << Name;
    EXPECT_TRUE(R.URL.empty());
    EXPECT_FALSE(R.EntryID.empty());
  }
}
TEST(WebImportMap, PrefixBacktrackingCannotEscapeOrFallBack) {
  const auto M =
      inspectImportMap("map", R"({"imports":{"p/":"./vendor/"}})", Base);
  ASSERT_EQ(M.Status, "ok");
  for (const auto Name : {"p/../x", "p/%2e%2e/x", "p//other.test/x"}) {
    const auto R = resolve(M, Name);
    EXPECT_EQ(R.Status, "blocked_import_map_backtracking") << Name;
    EXPECT_TRUE(R.URL.empty());
  }
}
TEST(WebImportMap, NonSpecialURLsAllowExactButNotPrefixMapping) {
  const auto M = inspectImportMap("map", R"({"imports":{
      "data:foo/":"./vendor/","data:exact":"./exact.js"}})",
                                  Base);
  ASSERT_EQ(M.Status, "ok");
  EXPECT_TRUE(M.OriginDependentKeys);
  EXPECT_EQ(resolve(M, "data:foo/child").URL, "data:foo/child");
  EXPECT_EQ(resolve(M, "data:exact").URL, "https://example.test/app/exact.js");
}
TEST(WebImportMap, EarlierDefinitionsWinAndLaterMapsCanExtendScopes) {
  const auto A =
      inspectImportMap("a", R"({"imports":{"p":"./first.js","stop":null},
      "scopes":{"./":{"s":"./first-scope.js"}}})",
                       Base);
  const auto B =
      inspectImportMap("b", R"({"imports":{"p":"./last.js","stop":"./x"},
      "scopes":{"./":{"s":"./last-scope.js","new":"./new.js"}}})",
                       Base);
  ASSERT_EQ(A.Status, "ok");
  ASSERT_EQ(B.Status, "ok");
  const ImportMap *Maps[]{&A, &B};
  const auto Get = [&](std::string_view Name) {
    return resolveImportMaps(Maps, Name, Base, [](uint64_t) {});
  };
  EXPECT_EQ(Get("p").URL, "https://example.test/app/first.js");
  EXPECT_EQ(Get("p").MapID, A.ID);
  EXPECT_EQ(Get("s").URL, "https://example.test/app/first-scope.js");
  EXPECT_EQ(Get("new").URL, "https://example.test/app/new.js");
  EXPECT_EQ(Get("new").MapID, B.ID);
  EXPECT_EQ(Get("stop").Status, "blocked_import_map");
}
TEST(WebImportMap, URLSpellingsQueriesAndFragmentsRemainDistinct) {
  const auto M = inspectImportMap("map", R"({"imports":{
      "./dep.js?x":"./one.js","./dep.js?y":"./two.js",
      "./%64ep.js?x":"./three.js","./dep.js#f":"./four.js"}})",
                                  Base);
  ASSERT_EQ(M.Status, "ok");
  EXPECT_EQ(resolve(M, "./dep.js?x").URL, "https://example.test/app/one.js");
  EXPECT_EQ(resolve(M, "./dep.js?y").URL, "https://example.test/app/two.js");
  EXPECT_EQ(resolve(M, "./%64ep.js?x").URL,
            "https://example.test/app/three.js");
  EXPECT_EQ(resolve(M, "./dep.js#f").URL, "https://example.test/app/four.js");
  EXPECT_EQ(resolve(M, "./dep.js").MatchKind, "default_url");
  EXPECT_EQ(resolve(M, "unknown").Status, "unmapped_bare_specifier");
}
TEST(WebImportMap, MalformedAndOrderDependentJSONRefusesAllMappings) {
  for (const auto Input :
       {"[]", "{", "{\"imports\":null}", "{\"scopes\":[]}", "{\"integrity\":7}",
        "{\"scopes\":{\"./\":7}}", R"({"imports":{"p":"./a","\u0070":"./b"}})",
        R"({"imports":{"./a/../p":"./a","./p":"./b"}})",
        R"({"scopes":{"./a/../":{},"./":{}}})",
        R"({"imports":{"p":"\ud800"}})"}) {
    const auto M = inspectImportMap("map", Input, Base);
    EXPECT_EQ(M.Status, "refused") << Input;
    EXPECT_FALSE(M.Reason.empty());
    EXPECT_TRUE(M.Imports.empty());
    EXPECT_TRUE(M.Scopes.empty());
  }
}
TEST(WebImportMap, IgnoredKeysAndIntegrityDoNotEstablishIntegrityVerification) {
  const auto M = inspectImportMap("map", R"({"imports":{"":"./x","p":"./p"},
      "future":{"anything":true},"integrity":{"./p":"sha256-PRIVATE","bare":"bad","./q":7}})",
                                  Base);
  ASSERT_EQ(M.Status, "ok");
  EXPECT_EQ(M.Imports.size(), 1u);
  EXPECT_TRUE(M.IntegrityPresent);
  EXPECT_EQ(M.IntegrityCount, 1u);
  EXPECT_EQ(M.IgnoredKeys, 4u);
  EXPECT_EQ(resolve(M, "p").Status, "url_candidate");
}
TEST(WebImportMap, AdmissionAndResolutionHaveIndependentRealBudgets) {
  const auto Huge =
      inspectImportMap("map", std::string(MaxImportMapBytes + 1, ' '), Base);
  EXPECT_EQ(Huge.Status, "budget_exceeded");
  std::string Text = "{\"imports\":{";
  for (uint64_t I = 0; I <= MaxImportMapRecords; ++I) {
    if (I)
      Text += ',';
    Text += '"' + std::to_string(I) + "\":null";
  }
  Text += "}}";
  const auto Many = inspectImportMap("map", Text, Base);
  EXPECT_EQ(Many.Status, "budget_exceeded");
  EXPECT_TRUE(Many.Imports.empty());
  EXPECT_LE(Many.Steps, MaxImportMapSteps);
  const auto M = inspectImportMap("map", R"({"imports":{"p":"./p"}})", Base);
  const ImportMap *Maps[]{&M};
  uint64_t Steps = 0;
  EXPECT_THROW(resolveImportMaps(Maps, "p", Base,
                                 [&](uint64_t N) {
                                   if (N > 32 - Steps)
                                     throw Error("test_budget_exceeded");
                                   Steps += N;
                                 }),
               Error);
}
TEST(WebImportMap, RepeatedLongBasesExhaustNormalizationBeforePublication) {
  std::string LongBase = "https://example.test/";
  for (unsigned I = 0; I < 15; ++I)
    LongBase += std::string(250, 'a') + '/';
  LongBase += "index.html";
  std::string Text = "{\"imports\":{";
  for (unsigned I = 0; I < 600; ++I) {
    if (I)
      Text += ',';
    Text += '"' + std::to_string(I) + "\":\"./dep.js\"";
  }
  Text += "}}";
  const auto M = inspectImportMap("map", Text, LongBase);
  EXPECT_EQ(M.Status, "budget_exceeded");
  EXPECT_EQ(M.Reason, "import_map_work_budget_exceeded");
  EXPECT_TRUE(M.Imports.empty());
  EXPECT_TRUE(M.Scopes.empty());
  EXPECT_LE(M.Steps, MaxImportMapSteps);
}
} // namespace
