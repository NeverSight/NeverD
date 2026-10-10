//===- ElectronEntryTests.cpp - Electron Entry tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Electron Entry tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/ElectronEntries.h"
#include "neverd/web/Session.h"

#include <algorithm>
#include <memory>

namespace {
using namespace neverd::web;
struct Model {
  std::string Text, Member;
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceModuleAnalysis M;
  SourceOrigins O;
  SourceValueAnalysis V;
  ElectronSource E;
  Model(std::string ID, std::string Path, std::string Input,
        std::string_view Type)
      : Text(std::move(Input)), Member(std::move(Path)) {
    S = inspectJavaScript(ID, Text, Type);
    EXPECT_EQ(S.ParseStatus, "parsed");
    B = analyzeSourceBindings(S);
    M = analyzeSourceModules(S, B);
    O = analyzeSourceOrigins(S, B, M);
    V = analyzeSourceValues(S);
    E = analyzeElectronSource(S, M, O, V);
  }
  uint32_t node(std::string_view Expression) const {
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      const auto &N = S.Nodes[I];
      if (std::string_view(Text).substr(N.Start, N.End - N.Start) == Expression)
        return I;
    }
    throw std::runtime_error("fixture expression missing");
  }
  SourcePaths paths(std::string_view Expression,
                    std::string_view AppDirectory = "app/") const {
    const auto Index = node(Expression);
    return analyzeSourcePaths(
        S, B, M, O, V,
        {"namespace", "manifest", Member, std::string(AppDirectory)},
        std::span(&Index, 1));
  }
  CapturedPathValue value(std::string_view Expression,
                          std::string_view AppDirectory = "app/") const {
    const auto A = paths(Expression, AppDirectory);
    EXPECT_EQ(A.Status, "partial") << A.Reason;
    return *A.Nodes.at(node(Expression));
  }
};

TEST(WebSourcePaths, CommonJSRootsAliasesTemplatesAndAppBaseRemainDistinct) {
  const Model M("source", "app/bin/main.js", R"JS(
    const path = require('node:path'); const {app} = require('electron');
    const base = __dirname; const name = 'preload.js';
    sink(path.join(base, '..', name)); sink(path.dirname(__filename));
    sink(path.resolve(app.getAppPath(), 'index.html'));
    sink(`${__dirname}/preload.js`); sink(__dirname + '/preload.js');
  )JS",
                "commonjs");
  auto P = M.value("path.join(base, '..', name)");
  EXPECT_EQ(P.Kind, CapturedPathKind::Path);
  EXPECT_EQ(P.Text, u"app/preload.js");
  EXPECT_NE(P.RootNode, NoSourceIndex);
  EXPECT_EQ(M.value("path.dirname(__filename)").Text, u"app/bin");
  EXPECT_EQ(M.value("path.resolve(app.getAppPath(), 'index.html')").Text,
            u"app/index.html");
  for (const auto Expression :
       {"`${__dirname}/preload.js`", "__dirname + '/preload.js'"}) {
    const auto Value = M.value(Expression);
    EXPECT_EQ(Value.Kind, CapturedPathKind::Path);
    EXPECT_EQ(capturedEntryTarget(Value, "app/", "window_preload").Path,
              "app/bin/preload.js");
  }
}

TEST(WebSourcePaths,
     JoinResolveNormalizationAndRawConcatenationHaveSeparateRules) {
  const Model M("source", "app/bin/main.js", R"JS(
    const path = require('path'); const {app} = require('electron');
    sink(path.resolve('ignored', app.getAppPath(), 'index.html'));
    sink(path.join('prefix', app.getAppPath(), 'index.html'));
    sink(path.resolve('index.html')); sink(path.join(__dirname, '/preload.js'));
    sink(__dirname + '/../preload.js');
    sink(path.normalize(__dirname + '/../preload.js'));
    sink(path.join(__dirname, 'preload.js/')); sink(path.dirname(__dirname));
    sink(path.posix.resolve(__dirname, 'preload.js'));
  )JS",
                "commonjs");
  EXPECT_EQ(
      M.value("path.resolve('ignored', app.getAppPath(), 'index.html')").Text,
      u"app/index.html");
  EXPECT_EQ(
      M.value("path.join('prefix', app.getAppPath(), 'index.html')").Reason,
      "unsupported_path_root_position");
  EXPECT_EQ(M.value("path.resolve('index.html')").Reason,
            "unknown_working_directory");
  EXPECT_EQ(M.value("path.join(__dirname, '/preload.js')").Text,
            u"app/bin/preload.js");
  const auto Raw = M.value("__dirname + '/../preload.js'");
  EXPECT_EQ(capturedEntryTarget(Raw, "app/", "window_preload").Status,
            "preload_path_requires_runtime_normalization");
  EXPECT_EQ(capturedEntryTarget(Raw, "app/", "renderer_file").Path,
            "app/preload.js");
  const auto Normalized =
      M.value("path.normalize(__dirname + '/../preload.js')");
  EXPECT_EQ(capturedEntryTarget(Normalized, "app/", "window_preload").Path,
            "app/preload.js");
  const auto Trailing = M.value("path.join(__dirname, 'preload.js/')");
  EXPECT_EQ(capturedEntryTarget(Trailing, "app/", "window_preload").Status,
            "preload_path_requires_runtime_normalization");
  EXPECT_EQ(capturedEntryTarget(Trailing, "app/", "renderer_file").Path,
            "app/bin/preload.js");
  EXPECT_EQ(M.value("path.posix.resolve(__dirname, 'preload.js')").Reason,
            "path_flavor_requires_target_context");
  const Model Root("root-source", "main.js",
                   "const path=require('path'); sink(path.dirname(__dirname)); "
                   "sink(__dirname + 'suffix');",
                   "commonjs");
  EXPECT_EQ(Root.value("path.dirname(__dirname)", "").Reason,
            "path_outside_captured_root");
  EXPECT_EQ(Root.value("__dirname + 'suffix'", "").Reason,
            "unknown_namespace_root_suffix");
}

TEST(WebSourcePaths, ModuleFileURLsKeepObjectAndStringKindsAndDirectoryBases) {
  const Model M("source", "app/bin/main.mjs", R"JS(
    import {fileURLToPath, URL as NodeURL} from 'node:url';
    import path from 'node:path';
    const file = fileURLToPath(import.meta.url); const dir = path.dirname(file);
    sink(path.join(dir, '..', 'preload.mjs'));
    sink(fileURLToPath(new NodeURL('../preload.mjs', import.meta.url)));
    sink(new URL('../index.html', import.meta.url).toString());
    sink(new URL('../index.html', import.meta.url).href);
    const folder = new URL('.', import.meta.url);
    sink(fileURLToPath(new URL('preload.mjs', folder)));
    sink(folder.toString()); sink(import.meta.dirname); sink(import.meta.filename);
    sink(new URL('x%2Fy', import.meta.url));
  )JS",
                "module");
  EXPECT_EQ(M.value("path.join(dir, '..', 'preload.mjs')").Text,
            u"app/preload.mjs");
  EXPECT_EQ(
      M.value("fileURLToPath(new NodeURL('../preload.mjs', import.meta.url))")
          .Text,
      u"app/preload.mjs");
  const auto URL =
      M.value("new URL('../index.html', import.meta.url).toString()");
  EXPECT_EQ(URL.Kind, CapturedPathKind::FileURLString);
  EXPECT_EQ(capturedEntryTarget(URL, "app/", "renderer_url").Path,
            "app/index.html");
  const auto Href = M.value("new URL('../index.html', import.meta.url).href");
  EXPECT_EQ(Href.Kind, CapturedPathKind::FileURLString);
  EXPECT_EQ(Href.Text, URL.Text);
  EXPECT_EQ(M.value("fileURLToPath(new URL('preload.mjs', folder))").Text,
            u"app/bin/preload.mjs");
  EXPECT_EQ(
      capturedEntryTarget(M.value("folder.toString()"), "app/", "renderer_url")
          .Status,
      "file_url_directory_reference");
  EXPECT_EQ(M.value("import.meta.dirname").Text, u"app/bin");
  EXPECT_EQ(M.value("import.meta.filename").Text, u"app/bin/main.mjs");
  EXPECT_EQ(M.value("new URL('x%2Fy', import.meta.url)").Reason,
            "unsupported_file_url_reference");
}

TEST(WebSourcePaths, ShadowedWrittenDynamicAndForeignCallsCannotSupplyRoots) {
  const Model M("source", "app/main.js", R"JS(
    const path = require('not-path'); sink(path.join(__dirname, 'x'));
    function f(__dirname) { sink(__dirname + '/x'); }
    const nativePath = require('path'); let dir = __dirname; dir = replacement;
    sink(nativePath.join(dir, 'x')); const a = b; const b = a; sink(nativePath.join(a, 'x'));
    with (scope) { sink(nativePath.join(__dirname, 'x')); }
  )JS",
                "commonjs");
  EXPECT_EQ(M.value("path.join(__dirname, 'x')").Kind,
            CapturedPathKind::Unknown);
  EXPECT_EQ(M.value("__dirname + '/x'").Kind, CapturedPathKind::Unknown);
  EXPECT_EQ(M.value("nativePath.join(dir, 'x')").Reason,
            "written_or_conflicting_path_binding");
  EXPECT_EQ(M.value("nativePath.join(a, 'x')").Reason,
            "cyclic_path_initializer");
  EXPECT_EQ(M.value("nativePath.join(__dirname, 'x')").Kind,
            CapturedPathKind::Unknown);
  const Model Written("s2", "app/main.js",
                      "__dirname = external; sink(__dirname + '/x');",
                      "commonjs");
  EXPECT_EQ(Written.value("__dirname + '/x'").Reason,
            "written_or_conflicting_path_binding");
  const Model Redeclared("s3", "app/main.js",
                         "function __dirname(){} sink(__dirname + '/x');",
                         "commonjs");
  EXPECT_EQ(Redeclared.value("__dirname + '/x'").Kind,
            CapturedPathKind::Unknown);
  const Model Eval("s4", "app/main.js", "eval(code); sink(__dirname);",
                   "commonjs");
  EXPECT_EQ(Eval.paths("__dirname").Status, "unavailable");
  const Model Script("s5", "app/main.js", "sink(__dirname);", "script");
  EXPECT_EQ(Script.value("__dirname").Kind, CapturedPathKind::Unknown);
  const Model URLWrite(
      "s6", "app/main.mjs",
      "URL = replacement; sink(new URL('x', import.meta.url));", "module");
  EXPECT_EQ(URLWrite.value("new URL('x', import.meta.url)").Kind,
            CapturedPathKind::Unknown);
}

TEST(WebSourcePaths, UnsafeEncodingEscapesExternalPathsAndModelBudgetsRefuse) {
  const Model M("source", "app/main.js", R"JS(const path=require('path');
    sink(path.join(__dirname, '..', '..', 'x')); sink(path.join(__dirname, 'CON'));
    sink(path.join(__dirname, '\ud800')); sink(path.join(__dirname, 'x\0y'));
    sink(path.join(__dirname, 'x\\y')); sink(path.resolve(__dirname, '/outside'));
    sink(path.join(__dirname, '\u03c0.js')); sink('index.html'); sink('../outside.html');
    sink('https://example.invalid/CANARY'); sink('C:/CANARY/preload.js');
  )JS",
                "commonjs");
  for (const auto Expr :
       {"path.join(__dirname, '..', '..', 'x')", "path.join(__dirname, 'CON')",
        "path.join(__dirname, '\\ud800')", "path.join(__dirname, 'x\\0y')",
        "path.join(__dirname, 'x\\\\y')"})
    EXPECT_EQ(M.value(Expr).Reason, "path_normalization_refused");
  EXPECT_EQ(M.value("path.resolve(__dirname, '/outside')").Reason,
            "external_absolute_path");
  EXPECT_EQ(capturedEntryTarget(M.value("path.join(__dirname, '\\u03c0.js')"),
                                "app/", "window_preload")
                .Path,
            "app/π.js");
  EXPECT_EQ(
      capturedEntryTarget(M.value("'index.html'"), "app/", "renderer_file")
          .Path,
      "app/index.html");
  EXPECT_EQ(
      capturedEntryTarget(M.value("'index.html'"), "app/", "window_preload")
          .Status,
      "preload_absolute_base_not_captured");
  EXPECT_EQ(
      capturedEntryTarget(M.value("'../outside.html'"), "app/", "renderer_file")
          .Status,
      "entry_outside_manifest_scope");
  EXPECT_EQ(capturedEntryTarget(M.value("'https://example.invalid/CANARY'"),
                                "app/", "renderer_url")
                .Status,
            "url_base_not_captured");
  auto Bad = M.O;
  Bad.SourceID = "other";
  const auto Node = M.node("'index.html'");
  const CapturedPathContext C{"ns", "manifest", M.Member, "app/"};
  auto A = analyzeSourcePaths(M.S, M.B, M.M, Bad, M.V, C, std::span(&Node, 1));
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Nodes.empty());
  std::vector<uint32_t> TooMany(10001, Node);
  A = analyzeSourcePaths(M.S, M.B, M.M, M.O, M.V, C, TooMany);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Nodes.empty());
}
TEST(WebSourcePaths, WholeAnalysisWorkAndAllocationBudgetsDiscardPaths) {
  for (const bool Allocation : {false, true}) {
    std::string Text = "const path=require('path'); const name='";
    Text += std::string(2000, 'a');
    Text += "';";
    for (unsigned I = 0; I < 700; ++I)
      Text +=
          Allocation ? "sink(name + '');" : "sink(path.join(__dirname, name));";
    const Model M("source", "app/main.js", Text, "commonjs");
    std::vector<uint32_t> Requested;
    for (uint32_t I = 0; I < M.S.Nodes.size(); ++I) {
      const auto &N = M.S.Nodes[I];
      const auto Expression =
          std::string_view(Text).substr(N.Start, N.End - N.Start);
      if (Expression ==
          (Allocation ? "name + ''" : "path.join(__dirname, name)"))
        Requested.push_back(I);
    }
    ASSERT_EQ(Requested.size(), 700U);
    const auto A =
        analyzeSourcePaths(M.S, M.B, M.M, M.O, M.V,
                           {"ns", "manifest", M.Member, "app/"}, Requested);
    EXPECT_EQ(A.Status, "budget_exceeded");
    EXPECT_EQ(A.Reason, "source_path_budget_exceeded");
    EXPECT_TRUE(A.Nodes.empty());
    EXPECT_LE(A.Steps, MaxCapturedPathSteps);
    EXPECT_LE(A.AllocatedUnits, MaxCapturedPathAllocatedUnits);
    if (Allocation) {
      EXPECT_GT(A.AllocatedUnits, MaxCapturedPathAllocatedUnits - 2000);
      EXPECT_LT(A.Steps, MaxCapturedPathSteps - 2000);
    } else {
      EXPECT_GT(A.Steps, MaxCapturedPathSteps - 2001);
      EXPECT_LT(A.AllocatedUnits, MaxCapturedPathAllocatedUnits - 2000);
    }
  }
}

TEST(WebSourcePaths, CanonicalContextAndRequestedEvidenceBindTheAnalysis) {
  const Model M("source", "app/main.js", "sink('a'); sink('b');", "commonjs");
  const auto A = M.paths("'a'");
  EXPECT_NE(A.ID, M.paths("'b'").ID);
  const auto Node = M.node("'a'");
  const std::vector<uint32_t> Duplicate{Node, Node};
  const auto D = analyzeSourcePaths(M.S, M.B, M.M, M.O, M.V,
                                    {"namespace", "manifest", M.Member, "app/"},
                                    Duplicate);
  EXPECT_EQ(A.ID, D.ID);
  EXPECT_EQ(A.Steps, D.Steps);
  EXPECT_EQ(A.AllocatedUnits, D.AllocatedUnits);
  std::vector<uint32_t> Both{Node, M.node("'b'")};
  const auto Ordered =
      analyzeSourcePaths(M.S, M.B, M.M, M.O, M.V,
                         {"namespace", "manifest", M.Member, "app/"}, Both);
  std::reverse(Both.begin(), Both.end());
  const auto Reordered =
      analyzeSourcePaths(M.S, M.B, M.M, M.O, M.V,
                         {"namespace", "manifest", M.Member, "app/"}, Both);
  EXPECT_EQ(Ordered.ID, Reordered.ID);
  EXPECT_EQ(Ordered.Steps, Reordered.Steps);
  EXPECT_EQ(Ordered.AllocatedUnits, Reordered.AllocatedUnits);
  for (const auto Base : {"app", "app//", "/app/", "app/../", "app\\/"}) {
    EXPECT_EQ(M.paths("'a'", Base).Reason, "invalid_captured_path_context");
    EXPECT_EQ(capturedEntryTarget(M.value("'a'"), Base, "renderer_file").Status,
              "invalid_captured_path_context");
  }
}

struct EntryScope {
  Snapshot Namespace;
  ElectronManifest Manifest;
  std::string ManifestBytes;
  std::vector<std::unique_ptr<Model>> Models;
  std::vector<ElectronEntryInput> Selected;
  explicit EntryScope(std::string_view Main = "bin/main.js",
                      std::string_view Type = "commonjs") {
    Namespace.ID = identity("entry-test-snapshot", {Main, Type});
    member("", "", true);
    ManifestBytes = "{\"main\":\"" + std::string(Main) + "\",\"type\":\"" +
                    std::string(Type) + "\"}";
    member("app/package.json", ManifestBytes);
  }
  std::string member(std::string_view Path, std::string_view Bytes,
                     bool Directory = false) {
    Artifact A;
    A.ID = identity("entry-test-member", {Namespace.ID, Path});
    A.MemberPath = Path;
    A.BlobHash = sha256(Bytes);
    A.Directory = Directory;
    Namespace.Artifacts.push_back(A);
    return A.ID;
  }
  Model &add(std::string Path, std::string Text,
             std::string_view Type = "commonjs") {
    auto M = std::make_unique<Model>(member(Path, Text), Path, Text, Type);
    Selected.push_back({&M->S, &M->E, &M->B, &M->M, &M->O, &M->V});
    Models.push_back(std::move(M));
    return *Models.back();
  }
  ElectronEntries run() {
    Manifest = analyzeElectronManifest(Namespace, Namespace.Artifacts[1].ID,
                                       ManifestBytes);
    return associateElectronEntries(Namespace, Manifest, Selected);
  }
};
template <typename F> void entryRefused(F Call, std::string_view Code) {
  try {
    (void)Call();
    FAIL() << "Expected refusal: " << Code;
  } catch (const Error &E) {
    EXPECT_EQ(E.what(), Code);
  }
}

TEST(WebElectronEntries,
     ExactCapturedEntriesUseAppAndSourceBasesAndSelectedIDs) {
  EntryScope S;
  S.add("app/bin/main.js", R"JS(
    const {BrowserWindow}=require('electron'); const path=require('path');
    const win=new BrowserWindow({webPreferences:{preload:path.join(__dirname,'..','preload.js')}});
    win.loadFile('index.html'); win.loadURL('https://example.invalid/CANARY');
    win.loadFile(dynamicName); win.loadFile('folder'); win.loadFile('missing');
    const other=new BrowserWindow({webPreferences:{preload:'preload.js'}});
  )JS");
  const auto &Preload = S.add("app/preload.js", "const data = 'CANARY';");
  const auto Page =
      S.member("app/index.html", "<script src='renderer.js'></script>");
  S.member("app/bin/index.html", "wrong source base");
  S.member("app/folder", "", true);
  const auto A = S.run();
  ASSERT_EQ(A.Sources.size(), 2U);
  ASSERT_EQ(A.Entries.size(), 7U);
  EXPECT_EQ(A.LinkedFiles, 2U);
  EXPECT_EQ(A.UnavailablePaths, 0U);
  std::vector<std::string> Status;
  for (const auto &E : A.Entries) {
    Status.push_back(E.Status);
    if (E.TargetArtifactID == Preload.S.ArtifactID) {
      EXPECT_EQ(E.TargetSourceID, Preload.S.ID);
      EXPECT_NE(E.RootNode, NoSourceIndex);
      EXPECT_NE(E.OperationNode, NoSourceIndex);
    } else if (!E.TargetArtifactID.empty()) {
      EXPECT_EQ(E.TargetArtifactID, Page);
      EXPECT_TRUE(E.TargetSourceID.empty());
    }
  }
  for (const auto Expected :
       {"url_base_not_captured", "unsupported_path_expression",
        "directory_not_entry_file", "no_exact_admitted_file",
        "preload_absolute_base_not_captured"})
    EXPECT_NE(std::find(Status.begin(), Status.end(), Expected), Status.end());
  std::reverse(S.Selected.begin(), S.Selected.end());
  EXPECT_EQ(S.run().ID, A.ID);
  S.Selected.erase(
      S.Selected.begin()); // Main-only selection still links the file.
  const auto MainOnly = S.run();
  EXPECT_NE(MainOnly.ID, A.ID);
  EXPECT_EQ(MainOnly.LinkedFiles, 2U);
  for (const auto &E : MainOnly.Entries)
    EXPECT_TRUE(E.TargetSourceID.empty());
}

TEST(WebElectronEntries, ModuleURLsStayInsideTheSelectedManifestNamespace) {
  EntryScope S("bin/main.mjs", "module");
  S.add("app/bin/main.mjs", R"JS(
    import {BrowserWindow} from 'electron'; import {fileURLToPath} from 'node:url';
    const win = new BrowserWindow({webPreferences:{preload:fileURLToPath(new URL('../preload.mjs', import.meta.url))}});
    win.loadURL(new URL('../index.html', import.meta.url).toString());
    win.loadURL(new URL('../../outside.html', import.meta.url).toString());
    win.loadURL(new URL('../folder/', import.meta.url).toString());
    win.loadFile();
  )JS",
        "module");
  S.member("app/preload.mjs", "export {};");
  S.member("app/index.html", "<p>captured</p>");
  S.member("outside.html", "outside application scope");
  S.member("app/folder", "", true);
  const auto A = S.run();
  EXPECT_EQ(A.LinkedFiles, 2U);
  ASSERT_EQ(A.Entries.size(), 5U);
  EXPECT_EQ(A.Entries[2].Status, "entry_outside_manifest_scope");
  EXPECT_EQ(A.Entries[3].Status, "file_url_directory_reference");
  EXPECT_EQ(A.Entries[4].Status, "missing_path_expression");
}

TEST(WebElectronEntries, ModelScopeAndAggregateRecordViolationsRefuse) {
  EntryScope S;
  auto &Main = S.add("app/bin/main.js",
                     "const {BrowserWindow}=require('electron'); const win=new "
                     "BrowserWindow(); win.loadFile('x');");
  const auto Valid = S.run();
  auto Bad = Main.O;
  Bad.ID = "other";
  S.Selected[0].Origins = &Bad;
  entryRefused([&] { return S.run(); }, "electron_entry_invalid_evidence");
  S.Selected[0].Origins = &Main.O;
  S.Selected.push_back(S.Selected[0]);
  entryRefused([&] { return S.run(); },
               "electron_ipc_duplicate_source_artifact");
  S.Selected.pop_back();
  Main.E.Boundaries[0].ID = "wrong-boundary";
  entryRefused([&] { return S.run(); }, "electron_entry_invalid_evidence");
  Main.E = analyzeElectronSource(Main.S, Main.M, Main.O, Main.V);
  S.add("different-app/renderer.js", "console.log('x');");
  entryRefused([&] { return S.run(); },
               "electron_ipc_source_outside_manifest_scope");
  S.Selected.pop_back();
  Main.E.Boundaries.resize(MaxElectronRecords + 1);
  entryRefused([&] { return S.run(); }, "electron_entry_budget_exceeded");
  EXPECT_FALSE(Valid.ID.empty());
}

TEST(WebElectronEntries,
     UnavailableOriginsRemainExplicitWithoutInventedEntries) {
  EntryScope S;
  S.add("app/bin/main.js",
        "eval(input); const {BrowserWindow}=require('electron'); const win=new "
        "BrowserWindow(); win.loadFile('index.html');");
  const auto A = S.run();
  EXPECT_EQ(A.UnavailablePaths, 1U);
  EXPECT_EQ(A.LinkedFiles, 0U);
  EXPECT_TRUE(A.Entries.empty());
  ASSERT_EQ(A.Sources.size(), 1U);
  EXPECT_EQ(A.Sources[0].PathStatus, "unavailable");
  EXPECT_EQ(A.Sources[0].PathReason, "source_path_evidence_unavailable");
}

TEST(WebElectronEntries, RepeatedAliasedPathsChargeTargetNormalizationWork) {
  EntryScope S;
  std::string Path;
  for (unsigned I = 0; I < 40; ++I)
    Path += std::string(99, 'a') + '/';
  Path += 'x';
  std::string Text = "const {BrowserWindow}=require('electron'); const win=new "
                     "BrowserWindow(); const name='" +
                     Path + "';";
  for (unsigned I = 0; I < 1200; ++I)
    Text += "win.loadFile(name);";
  const auto &M = S.add("app/bin/main.js", Text);
  ASSERT_EQ(M.E.Status, "partial");
  ASSERT_EQ(M.E.Boundaries.size(), 1201U);
  entryRefused([&] { return S.run(); }, "electron_entry_budget_exceeded");
}
} // namespace
