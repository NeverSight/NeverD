#include "gtest/gtest.h"

#include "neverd/web/ElectronIPC.h"
#include "neverd/web/Session.h"

#include <algorithm>
#include <memory>

namespace {
using namespace neverd::web;
struct Scope {
  Snapshot Namespace;
  ElectronManifest Manifest;
  std::vector<std::unique_ptr<SourceAnalysis>> Sources;
  std::vector<std::unique_ptr<ElectronSource>> Evidence;
  std::vector<ElectronIPCInput> Selected;
  std::string ManifestBytes = R"({"main":"main.js"})";
  explicit Scope(std::string_view Name = "test-app") {
    Namespace.ID = identity("ipc-test-snapshot", {Name});
    Artifact Root;
    Root.ID = identity("ipc-test-root", {Name});
    Root.Directory = true;
    Namespace.Artifacts.push_back(Root);
    Artifact M;
    M.ID = identity("ipc-test-manifest", {Name});
    M.MemberPath = "app/package.json";
    M.BlobHash = sha256(ManifestBytes);
    Namespace.Artifacts.push_back(M);
  }
  void add(std::string_view Path, std::string_view Text,
           std::string_view Type = "commonjs") {
    Artifact Member;
    Member.ID = identity("ipc-test-member", {Namespace.ID, Path});
    Member.MemberPath = Path;
    Member.BlobHash = sha256(Text);
    Namespace.Artifacts.push_back(Member);
    auto S = std::make_unique<SourceAnalysis>(
        inspectJavaScript(Member.ID, Text, Type));
    EXPECT_EQ(S->ParseStatus, "parsed");
    const auto Bindings = analyzeSourceBindings(*S);
    const auto Modules = analyzeSourceModules(*S, Bindings);
    const auto Origins = analyzeSourceOrigins(*S, Bindings, Modules);
    const auto Values = analyzeSourceValues(*S);
    auto E = std::make_unique<ElectronSource>(
        analyzeElectronSource(*S, Modules, Origins, Values));
    Selected.push_back({S.get(), E.get()});
    Sources.push_back(std::move(S));
    Evidence.push_back(std::move(E));
  }
  ElectronIPC run() {
    Manifest = analyzeElectronManifest(Namespace, Namespace.Artifacts[1].ID,
                                       ManifestBytes);
    return correlateElectronIPC(Namespace, Manifest, Selected);
  }
};
template <typename F> void refused(F Call, std::string_view Code) {
  try {
    (void)Call();
    FAIL() << "Expected refusal: " << Code;
  } catch (const Error &E) {
    EXPECT_EQ(E.what(), Code);
  }
}

TEST(WebElectronIPC, ExplicitSelectionJoinsChannelsAndRetainsUnresolvedCalls) {
  Scope S;
  S.add("app/main.js", R"JS(const {ipcMain,BrowserWindow} = require('electron');
    ipcMain.handle('CANARY-request', () => {});
    ipcMain.handleOnce('CANARY-request', () => {});
    ipcMain.on('CANARY-push', () => {});
    const win = new BrowserWindow(); win.webContents.send('CANARY-reply');
    ipcMain.removeHandler('CANARY-request');)JS");
  S.add("app/preload.js",
        R"JS(const {ipcRenderer,contextBridge} = require('electron');
    ipcRenderer.invoke('CANARY-' + 'request'); ipcRenderer.send('CANARY-push');
    ipcRenderer.on('CANARY-reply', () => {}); ipcRenderer.removeAllListeners('CANARY-reply');
    ipcRenderer.invoke(dynamicValue); contextBridge.exposeInMainWorld('CANARY-request', {});)JS");
  const auto A = S.run();
  ASSERT_EQ(A.Sources.size(), 2U);
  ASSERT_EQ(A.Channels.size(), 3U);
  EXPECT_EQ(A.Endpoints.size(), 10U);
  EXPECT_EQ(A.UnresolvedEndpoints, 1U);
  EXPECT_EQ(A.UnavailableSources, 0U);
  uint64_t InvokePairs = 0, PushPairs = 0, ReplyPairs = 0;
  for (const auto &C : A.Channels) {
    InvokePairs += C.Counts[0] * C.Counts[1];
    PushPairs += C.Counts[2] * C.Counts[3];
    ReplyPairs += C.Counts[4] * C.Counts[5];
  }
  EXPECT_EQ(InvokePairs, 2U);
  EXPECT_EQ(PushPairs, 1U);
  EXPECT_EQ(ReplyPairs, 1U);
  for (const auto &E : A.Endpoints) {
    const auto &Source = A.Sources.at(E.Source);
    EXPECT_FALSE(Source.SourceID.empty());
    EXPECT_FALSE(E.ID.empty());
  }
  std::reverse(S.Selected.begin(), S.Selected.end());
  const auto Reordered = S.run();
  EXPECT_EQ(A.ID, Reordered.ID);
  for (size_t I = 0; I < A.Channels.size(); ++I)
    EXPECT_EQ(A.Channels[I].ID, Reordered.Channels[I].ID);
  S.Selected.pop_back();
  // A selection without the main source refuses instead of guessing a scope.
  if (S.Selected[0].Source->ArtifactID == A.MainArtifactID)
    EXPECT_NE(S.run().ID, A.ID);
  else
    refused([&] { return S.run(); }, "electron_ipc_main_source_required");
}

TEST(WebElectronIPC, UTF16EqualityPreservesNulSurrogatesAndNormalization) {
  Scope S;
  S.add("app/main.js", R"JS(const {ipcMain} = require('electron');
    ipcMain.handle('a\0b', f); ipcMain.handle('\ud800', f);
    ipcMain.handle('\u00e9', f);)JS");
  S.add("app/preload.js", R"JS(const {ipcRenderer} = require('electron');
    ipcRenderer.invoke('a\x00b'); ipcRenderer.invoke('\uD800');
    ipcRenderer.invoke('e\u0301');)JS");
  const auto A = S.run();
  ASSERT_EQ(A.Channels.size(), 4U);
  uint64_t Pairs = 0;
  for (const auto &C : A.Channels)
    Pairs += C.Counts[0] * C.Counts[1];
  EXPECT_EQ(Pairs, 2U);
  Scope Other("other-app");
  Other.add("app/main.js",
            "const {ipcMain}=require('electron'); ipcMain.handle('a\\0b', f);");
  const auto B = Other.run();
  EXPECT_NE(A.ID, B.ID);
  for (const auto &C : A.Channels)
    EXPECT_NE(C.ID, B.Channels[0].ID);
}

TEST(WebElectronIPC, ScopeModeAndEvidenceMismatchFailBeforePublishing) {
  Scope S;
  S.add("app/main.js",
        "const {ipcMain}=require('electron'); ipcMain.handle('x', f);");
  S.add("application/other.js",
        "const {ipcRenderer}=require('electron'); ipcRenderer.invoke('x');");
  refused([&] { return S.run(); },
          "electron_ipc_source_outside_manifest_scope");
  S.Selected.pop_back();
  const auto Valid = S.run();
  S.Selected.push_back(S.Selected[0]);
  refused([&] { return S.run(); }, "electron_ipc_duplicate_source_artifact");
  S.Selected.pop_back();
  const auto Hash = S.Sources[0]->BlobHash;
  S.Sources[0]->BlobHash = "different";
  refused([&] { return S.run(); }, "electron_ipc_invalid_evidence");
  S.Sources[0]->BlobHash = Hash;
  S.Manifest.ID = "wrong-manifest";
  refused(
      [&] { return correlateElectronIPC(S.Namespace, S.Manifest, S.Selected); },
      "electron_ipc_manifest_mismatch");
  EXPECT_EQ(S.run().ID, Valid.ID);
  Scope Mode;
  Mode.add("app/main.js",
           "import {ipcMain} from 'electron'; ipcMain.handle('x', f);",
           "module");
  refused([&] { return Mode.run(); }, "electron_ipc_main_source_type_mismatch");
  S.Namespace.Artifacts.push_back(S.Namespace.Artifacts.back());
  refused([&] { return S.run(); }, "electron_ipc_ambiguous_namespace");
}

TEST(WebElectronIPC, UnavailableSourceStaysVisibleWithoutInventingEndpoints) {
  Scope S;
  S.add("app/main.js",
        "const {ipcMain}=require('electron'); ipcMain.handle('x', f);");
  S.add("app/preload.js", "const {ipcRenderer}=require('electron'); "
                          "eval(code); ipcRenderer.invoke('x');");
  const auto A = S.run();
  EXPECT_EQ(A.Sources.size(), 2U);
  EXPECT_EQ(A.UnavailableSources, 1U);
  EXPECT_EQ(A.Endpoints.size(), 1U);
  EXPECT_EQ(A.Channels[0].Counts[1], 0U);
  EXPECT_EQ(S.Evidence[1]->Status, "unavailable");
  EXPECT_EQ(S.Evidence[1]->Reason, "electron_source_origins_unavailable");
}

TEST(WebElectronIPC, AggregateRecordStringAndComparisonBudgetsAreAtomic) {
  Scope S;
  S.add("app/main.js",
        "const {ipcMain}=require('electron'); ipcMain.handle('x', f);");
  const auto Original = *S.Evidence[0];
  S.Evidence[0]->Boundaries.resize(MaxElectronIPCEndpoints + 1);
  refused([&] { return S.run(); }, "electron_ipc_record_budget_exceeded");
  *S.Evidence[0] = Original;
  S.Evidence[0]->Boundaries[0].Text.assign(MaxElectronIPCUnits + 1, u'x');
  refused([&] { return S.run(); }, "electron_ipc_string_budget_exceeded");
  *S.Evidence[0] = Original;
  EXPECT_EQ(S.run().Endpoints.size(), 1U);
  Scope Work;
  std::string Text = "const {ipcMain}=require('electron');";
  for (unsigned I = 0; I < 64; ++I)
    Text += "ipcMain.handle('" + std::to_string(I) + "',f);";
  Work.add("app/main.js", Text);
  ASSERT_EQ(Work.Evidence[0]->Boundaries.size(), 64U);
  for (unsigned I = 0; I < 64; ++I) {
    auto &Channel = Work.Evidence[0]->Boundaries[I].Text;
    Channel.assign(8192, u'x');
    Channel.back() = char16_t(I);
  }
  refused([&] { return Work.run(); }, "electron_ipc_work_budget_exceeded");
}
} // namespace
