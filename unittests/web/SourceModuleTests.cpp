//===- SourceModuleTests.cpp - Source Module tests ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source Module tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Session.h"
#include "neverd/web/SourceModules.h"

#include <algorithm>
#include <set>

namespace {
using namespace neverd::web;
constexpr auto None = NoSourceIndex;

struct Module {
  SourceAnalysis Source;
  SourceBindingAnalysis Bindings;
  SourceModuleAnalysis Modules;
  explicit Module(std::string_view Text, std::string_view Type = "module")
      : Source(inspectJavaScript("test-artifact", Text, Type)),
        Bindings(analyzeSourceBindings(Source)),
        Modules(analyzeSourceModules(Source, Bindings)) {}
  const SourceModuleExport *exported(std::u16string_view Name) const {
    for (const auto &E : Modules.Exports)
      if (E.ExportedName != None && Modules.Names[E.ExportedName] == Name)
        return &E;
    return nullptr;
  }
};

TEST(WebModules, EsmRequestsAndBindingsKeepOccurrenceIdentityAndPrivateNames) {
  const std::string Text = R"JS(
    import './side.js';
    import main, { original as local } from './dep.js';
    import * as space from './dep.js';
    export { local as exposed }; export { main }; export { space };
    export { foreign as remote } from './other.js';
    export * from './star.js'; export * as ns from './ns.js';
  )JS";
  const Module M(Text), Again(Text);
  ASSERT_EQ(M.Source.ParseStatus, "parsed");
  ASSERT_EQ(M.Modules.Status, "ok");
  ASSERT_EQ(M.Modules.Requests.size(), 6);
  ASSERT_EQ(M.Modules.Imports.size(), 3);
  ASSERT_EQ(M.Modules.Exports.size(), 6);
  EXPECT_EQ(M.Modules.Requests[1].Specifier, M.Modules.Requests[2].Specifier);
  EXPECT_NE(M.Modules.Requests[1].ID, M.Modules.Requests[2].ID);
  for (size_t I = 0; I < M.Modules.Requests.size(); ++I) {
    const auto &R = M.Modules.Requests[I];
    EXPECT_EQ(R.ID, Again.Modules.Requests[I].ID);
    const auto &N = M.Source.Nodes[R.SpecifierNode];
    EXPECT_EQ(Text.substr(N.Start, N.End - N.Start),
              "'" +
                  std::string(M.Modules.Names[R.Specifier].begin(),
                              M.Modules.Names[R.Specifier].end()) +
                  "'");
  }
  ASSERT_NE(M.exported(u"exposed"), nullptr);
  EXPECT_EQ(M.exported(u"exposed")->Binding, M.Modules.Imports[1].Binding);
  EXPECT_EQ(M.exported(u"remote")->LinkStatus, "foreign_module_unlinked");
  EXPECT_EQ(M.exported(u"ns")->Kind, "namespace_reexport");
  EXPECT_EQ(M.Modules.Imports[2].Kind, "namespace");
  EXPECT_EQ(M.Modules.Imports[2].ImportedName, None);
}

TEST(WebModules, ExportDeclarationsVisitBindingPatternsAndNeverNestedLocals) {
  const Module M(R"JS(
    export const {key: local = (() => { const hidden = 1; return hidden; })(),
                  ...rest} = input;
    export let [first,,...tail] = other;
    export function fn(param) { var hidden; return param; }
    export class C { method(param) { const hidden = 0; } }
    export default function named(param) { return param; }
  )JS");
  ASSERT_EQ(M.Modules.Status, "ok");
  const std::set<std::u16string> Expected = {
      u"local", u"rest", u"first", u"tail", u"fn", u"C", u"default"};
  std::set<std::u16string> Names;
  for (const auto &E : M.Modules.Exports) {
    ASSERT_NE(E.Binding, None);
    Names.insert(M.Modules.Names[E.ExportedName]);
    EXPECT_EQ(M.Bindings.Bindings[E.Binding].Scope, 0);
  }
  EXPECT_EQ(Names, Expected);
  ASSERT_NE(M.exported(u"default"), nullptr);
  EXPECT_EQ(M.exported(u"default")->Kind, "default_declaration");
  EXPECT_EQ(M.Modules.Names[M.exported(u"default")->LocalName], u"named");
}

TEST(WebModules, DefaultExpressionsDoNotInventLexicalBindings) {
  for (const auto Text : {"export default 1;", "export default function() {};",
                          "export default class {};"}) {
    const Module M(Text);
    ASSERT_EQ(M.Modules.Status, "ok") << Text;
    ASSERT_EQ(M.Modules.Exports.size(), 1);
    EXPECT_EQ(M.Modules.Exports[0].Binding, None);
    EXPECT_EQ(M.Modules.Exports[0].Kind, "default_expression");
    EXPECT_EQ(M.Modules.Exports[0].LinkStatus, "expression_or_unbound");
  }
  const Module NamedClass(
      "export default class InnerName { method() { return InnerName; } }");
  ASSERT_EQ(NamedClass.Modules.Status, "ok");
  ASSERT_EQ(NamedClass.Modules.Exports.size(), 1);
  const auto B = NamedClass.Modules.Exports[0].Binding;
  ASSERT_NE(B, None);
  EXPECT_EQ(NamedClass.Bindings.Bindings[B].Kind, "class");
  EXPECT_EQ(NamedClass.Bindings.Bindings[B].Scope, 0);
}

TEST(WebModules, DynamicImportsRetainBoundariesEvenWhenTheSpecifierIsLiteral) {
  const Module M(R"JS(
    import('./literal.js'); import('./' + part); import(`./template.js`);
    import('./data.json', { with: { type: 'json' } });
  )JS");
  ASSERT_EQ(M.Modules.Status, "ok");
  ASSERT_EQ(M.Modules.Requests.size(), 4);
  EXPECT_EQ(M.Modules.Requests[0].SpecifierStatus, "literal");
  EXPECT_EQ(M.Modules.Requests[1].SpecifierStatus, "dynamic");
  EXPECT_EQ(M.Modules.Requests[2].SpecifierStatus, "dynamic");
  EXPECT_NE(M.Modules.Requests[3].AttributesNode, None);
  const auto Links = linkSourceModules(M.Source, M.Modules, {});
  ASSERT_EQ(Links.Requests.size(), 4);
  for (const auto &L : Links.Requests) {
    EXPECT_EQ(L.Status, "dynamic_import_boundary");
    EXPECT_TRUE(L.ArtifactID.empty());
  }
}

TEST(WebModules, RequireSpellingAndWrapperBindingNeverProveARuntimeLoader) {
  const Module M(R"JS(
    require('./one'); require.resolve('./two'); require['resolve']('./three');
    function local(require) { require('./shadow'); }
    require = replacement; require('./mutated'); require?.('./optional');
    require(...args); require(); const alias = require; alias('./alias');
    object.require('./property');
  )JS",
                 "commonjs");
  ASSERT_EQ(M.Modules.Status, "ok");
  ASSERT_EQ(M.Modules.Requests.size(), 8);
  EXPECT_EQ(M.Modules.Requests[0].CalleeEvidence,
            "caller_selected_commonjs_parameter");
  EXPECT_EQ(M.Modules.Requests[1].Kind, "require_resolve_candidate");
  EXPECT_EQ(M.Modules.Requests[2].Kind, "require_resolve_candidate");
  EXPECT_EQ(M.Modules.Requests[3].CalleeEvidence, "other_lexical_binding");
  EXPECT_NE(M.Modules.Requests[0].CalleeBinding,
            M.Modules.Requests[3].CalleeBinding);
  EXPECT_EQ(M.Modules.Requests[4].CalleeEvidence,
            "caller_selected_commonjs_parameter");
  EXPECT_TRUE(M.Modules.Requests[5].Optional);
  EXPECT_EQ(M.Modules.Requests[6].SpecifierStatus, "dynamic");
  EXPECT_EQ(M.Modules.Requests[7].SpecifierStatus, "missing");
  const auto Links = linkSourceModules(M.Source, M.Modules, {});
  ASSERT_EQ(Links.Requests.size(), M.Modules.Requests.size());
  for (const auto &L : Links.Requests)
    EXPECT_EQ(L.Status, "unverified_callee");
  const Module External("require('./x');", "script");
  ASSERT_EQ(External.Modules.Requests.size(), 1);
  EXPECT_EQ(External.Modules.Requests[0].CalleeEvidence, "external");
  const Module With("with (object) { require('./x'); }", "script");
  ASSERT_EQ(With.Modules.Requests.size(), 1);
  EXPECT_EQ(With.Modules.Requests[0].CalleeEvidence, "dynamic_with");
}

TEST(WebModules, ImportAssertionsRemainPrivateVersionedEvidence) {
  const Module M("import data from './data.json' assert { type: 'json', "
                 "secret: 'TOKEN' };");
  ASSERT_EQ(M.Modules.Status, "ok");
  ASSERT_EQ(M.Modules.Attributes.size(), 2);
  EXPECT_EQ(M.Modules.Names[M.Modules.Attributes[0].Key], u"type");
  EXPECT_EQ(M.Modules.Names[M.Modules.Attributes[1].Value], u"TOKEN");
  EXPECT_EQ(M.Modules.Attributes[1].Request, 0);
  const Module Duplicate(
      "import './x' assert { type: 'json', '\\u0074ype': 'text' };");
  ASSERT_EQ(Duplicate.Modules.Status, "partial");
  ASSERT_EQ(Duplicate.Modules.Attributes.size(), 2);
  EXPECT_TRUE(Duplicate.Modules.Attributes[0].Conflicting);
  EXPECT_TRUE(Duplicate.Modules.Attributes[1].Conflicting);
  EXPECT_EQ(Duplicate.Modules.Diagnostics[0].Code,
            "duplicate_import_assertion");
  // The pinned parser is not silently upgraded to a different grammar.
  const Module Modern("import data from './data.json' with { type: 'json' };");
  EXPECT_NE(Modern.Source.ParseStatus, "parsed");
  EXPECT_TRUE(Modern.Modules.Requests.empty());
}

TEST(WebModules, DuplicateAndMissingExportsAreNotSilentlyLinked) {
  const Module Duplicate(
      "const value = 1; export {value as same}; export {value as same};");
  ASSERT_EQ(Duplicate.Modules.Status, "partial");
  ASSERT_EQ(Duplicate.Modules.Exports.size(), 2);
  EXPECT_TRUE(Duplicate.Modules.Exports[0].Conflicting);
  EXPECT_TRUE(Duplicate.Modules.Exports[1].Conflicting);
  EXPECT_EQ(Duplicate.Modules.Diagnostics[0].Code, "duplicate_export_name");
  const Module Missing("export { missing };");
  ASSERT_EQ(Missing.Modules.Status, "partial");
  ASSERT_EQ(Missing.Modules.Exports.size(), 1);
  EXPECT_EQ(Missing.Modules.Exports[0].Binding, None);
  EXPECT_EQ(Missing.Modules.Diagnostics[0].Code, "unbound_export_name");
}

Snapshot input(std::string_view Origin = "src/main.js") {
  Snapshot S;
  S.ID = "test-snapshot";
  auto Add = [&](std::string ID, std::string Path, bool Directory = false) {
    Artifact A;
    A.ID = ID;
    A.MemberPath = Path;
    A.Directory = Directory;
    S.Artifacts.push_back(std::move(A));
  };
  Add("root", "", true);
  Add("test-artifact", std::string(Origin));
  Add("dep", "src/dep.js");
  Add("root-file", "root.js");
  Add("dir", "src/folder", true);
  Add("index", "src/folder/index.js");
  Add("unicode", "src/雪.js");
  return S;
}

TEST(WebModules, LocalLinksUseOnlyExactAdmittedMembersWithoutLoaderGuesses) {
  const Module M(R"JS(
    import './dep.js'; export { x } from '../root.js';
    import './dep'; import './folder'; import './missing.js'; import './雪.js';
    import './dep.js';
  )JS");
  ASSERT_EQ(M.Modules.Status, "ok");
  const auto Links = linkSourceModules(M.Source, M.Modules, input());
  ASSERT_EQ(Links.Status, "ok");
  ASSERT_EQ(Links.Requests.size(), 7);
  EXPECT_EQ(Links.Requests[0].Status, "exact_admitted_file_candidate");
  EXPECT_EQ(Links.Requests[0].ArtifactID, "dep");
  EXPECT_EQ(Links.Requests[1].ArtifactID, "root-file");
  EXPECT_EQ(Links.Requests[2].Status, "not_in_snapshot");
  EXPECT_EQ(Links.Requests[3].Status, "directory_target");
  EXPECT_EQ(Links.Requests[4].Status, "not_in_snapshot");
  EXPECT_EQ(Links.Requests[5].ArtifactID, "unicode");
  EXPECT_EQ(Links.Requests[6].ArtifactID, "dep");
  auto Other = input();
  Other.ID = "other-snapshot";
  EXPECT_NE(Links.ID, linkSourceModules(M.Source, M.Modules, Other).ID);
}

TEST(WebModules, LocalLinksRefuseEscapesUrlsEncodedPathsAndUntrustedOrigins) {
  const Module M(R"JS(
    import '../../root.js'; import './dep.js?query'; import './dep.js#fragment';
    import './%64ep.js'; import './sub\\dep.js'; import 'node:fs';
    import 'https://example.invalid/file.js'; import 'pkg'; import '/root.js';
    import './\uD800.js'; import './\u0000.js'; import './folder/';
  )JS");
  ASSERT_EQ(M.Modules.Status, "ok");
  const auto Links = linkSourceModules(M.Source, M.Modules, input());
  ASSERT_EQ(Links.Requests.size(), 12);
  for (const auto &L : Links.Requests) {
    EXPECT_NE(L.Status, "exact_admitted_file_candidate");
    EXPECT_TRUE(L.ArtifactID.empty());
  }
  const Module Good("import './dep.js';");
  auto Single = input("");
  Single.Artifacts.erase(Single.Artifacts.begin());
  EXPECT_EQ(
      linkSourceModules(Good.Source, Good.Modules, Single).Requests[0].Status,
      "directory_origin_unavailable");
  auto Derived = Good.Source;
  Derived.ArtifactID = "derived-map-source";
  EXPECT_EQ(
      linkSourceModules(Derived, Good.Modules, input()).Requests[0].Status,
      "directory_origin_unavailable");
}

TEST(WebModules, InvalidModelsAndMismatchedAnalysesFailWithoutPartialEvidence) {
  Module M("import './x';");
  auto Other = M.Bindings;
  Other.SourceID = "different";
  EXPECT_THROW(analyzeSourceModules(M.Source, Other), Error);
  Other = M.Bindings;
  Other.Status = "budget_exceeded";
  const auto Unavailable = analyzeSourceModules(M.Source, Other);
  EXPECT_EQ(Unavailable.Status, "unavailable");
  EXPECT_TRUE(Unavailable.Requests.empty());
  auto Broken = M.Source;
  Broken.Nodes[0].Children[0].Index = uint32_t(Broken.Nodes.size());
  const auto Rejected = analyzeSourceModules(Broken, M.Bindings);
  EXPECT_EQ(Rejected.Status, "unsupported");
  EXPECT_TRUE(Rejected.Requests.empty());
  auto Changed = M.Modules;
  Changed.SourceID = "different";
  EXPECT_THROW(linkSourceModules(M.Source, Changed, input()), Error);
}

TEST(WebModules, ResourceExhaustionClearsTheWholeInventoryAndLinkSet) {
  Module M("import './x'; import './y';");
  auto Large = M.Source;
  auto &Literal = Large.Nodes[M.Modules.Requests[1].SpecifierNode];
  for (auto &A : Literal.Attributes)
    if (A.Field == "value")
      A.Value = std::u16string(MaxJavaScriptModuleNameUnits + 1, u'x');
  const auto Rejected = analyzeSourceModules(Large, M.Bindings);
  EXPECT_EQ(Rejected.Status, "budget_exceeded");
  EXPECT_TRUE(Rejected.Requests.empty());
  EXPECT_TRUE(Rejected.Names.empty());
  EXPECT_EQ(Rejected.Diagnostics[0].Code, "source_module_name_budget_exceeded");
  for (auto &A : Literal.Attributes)
    if (A.Field == "value")
      A.Value = std::u16string(MaxJavaScriptModuleSteps, u'x');
  const auto Work = analyzeSourceModules(Large, M.Bindings);
  EXPECT_EQ(Work.Status, "budget_exceeded");
  EXPECT_TRUE(Work.Requests.empty());
  EXPECT_EQ(Work.Diagnostics[0].Code, "source_module_budget_exceeded");
  auto Long = M.Modules;
  Long.Names[Long.Requests[1].Specifier] =
      std::u16string(MaxJavaScriptModuleSteps, u'x');
  const auto Links = linkSourceModules(M.Source, Long, input());
  EXPECT_EQ(Links.Status, "budget_exceeded");
  EXPECT_TRUE(Links.Requests.empty());
}
} // namespace
