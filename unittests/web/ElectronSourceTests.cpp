#include "gtest/gtest.h"

#include "neverd/web/Electron.h"

#include <algorithm>

namespace {
using namespace neverd::web;
struct Inspection {
  std::string Text;
  SourceAnalysis Source;
  SourceBindingAnalysis Bindings;
  SourceModuleAnalysis Modules;
  SourceOrigins Origins;
  SourceValueAnalysis Values;
  ElectronSource Electron;
  explicit Inspection(std::string Input, std::string_view Type = "commonjs")
      : Text(std::move(Input)) {
    Source = inspectJavaScript(identity("electron-test", {Text}), Text, Type);
    EXPECT_EQ(Source.ParseStatus, "parsed");
    Bindings = analyzeSourceBindings(Source);
    Modules = analyzeSourceModules(Source, Bindings);
    Origins = analyzeSourceOrigins(Source, Bindings, Modules);
    Values = analyzeSourceValues(Source);
    Electron = analyzeElectronSource(Source, Modules, Origins, Values);
  }
  std::string_view text(uint32_t I) const {
    const auto &N = Source.Nodes.at(I);
    return std::string_view(Text).substr(N.Start, N.End - N.Start);
  }
  const ElectronBoundary *find(std::string_view Kind) const {
    const auto I =
        std::find_if(Electron.Boundaries.begin(), Electron.Boundaries.end(),
                     [&](const auto &B) { return B.Kind == Kind; });
    return I == Electron.Boundaries.end() ? nullptr : &*I;
  }
};

TEST(WebSourceOrigins, ImportsAliasesAndDestructuringRetainModuleRequests) {
  Inspection I(R"JS(import {ipcMain as main} from 'electron';
    import * as electron from 'electron';
    const alias = main; const {contextBridge: bridge} = electron;
    alias.handle('private-channel', handler); bridge.exposeInMainWorld('private-api', {});
    const {ipcRenderer: {send: emit}} = electron; emit('private-channel');
    typeof alias; export {alias};)JS",
               "module");
  EXPECT_EQ(I.Origins.Status, "partial");
  ASSERT_EQ(I.Electron.Boundaries.size(), 3U);
  for (const auto &B : I.Electron.Boundaries)
    EXPECT_EQ(B.OriginEvidence, "esm_import_syntax");
  EXPECT_NE(I.find("ipc_main_handle"), nullptr);
  EXPECT_NE(I.find("bridge_main_world"), nullptr);
  EXPECT_NE(I.find("ipc_renderer_send"), nullptr);
}

TEST(WebSourceOrigins,
     WritesShadowingDynamicLookupAndRequireReplacementRefuseLinks) {
  Inspection I(R"JS(const {ipcMain} = require('electron');
    ipcMain.handle('yes', handler);
    function f(ipcMain) { ipcMain.handle('shadow', handler); }
    let changed = ipcMain; changed = unrelated; changed.handle('no', handler);
    with (object) { ipcMain.handle('dynamic', handler); }
    function g(require) { const {ipcMain} = require('electron'); ipcMain.handle('no', handler); }
    const cycleA = cycleB; const cycleB = cycleA; cycleA.handle('no', handler);
  )JS");
  ASSERT_EQ(I.Electron.Boundaries.size(), 1U);
  EXPECT_EQ(I.Electron.Boundaries[0].Text, u"yes");
  Inspection Written(
      R"JS(const electron = require('electron'); require = replacement;
    electron.ipcMain.handle('no', handler);)JS");
  EXPECT_TRUE(Written.Electron.Boundaries.empty());
  Inspection ExternalWrite(R"JS(require = replacement;
    const {ipcMain} = require('electron'); ipcMain.handle('no', handler);)JS",
                           "script");
  EXPECT_TRUE(ExternalWrite.Electron.Boundaries.empty());
  Inspection WithWrite(R"JS(with (object) { require = replacement; }
    const {ipcMain} = require('electron'); ipcMain.handle('no', handler);)JS");
  EXPECT_TRUE(WithWrite.Electron.Boundaries.empty());
  Inspection Eval(R"JS(const {ipcMain} = require('electron'); eval(code);
    ipcMain.handle('unknown', handler);)JS");
  EXPECT_EQ(Eval.Origins.Status, "unavailable");
  EXPECT_EQ(Eval.Origins.Reason, "source_origin_dynamic_eval");
  EXPECT_TRUE(Eval.Electron.Boundaries.empty());
}

TEST(WebSourceOrigins,
     CallsDoNotPropagateObjectsAndForeignModulesAreNotElectron) {
  Inspection I(R"JS(const electron = require('different-module');
    electron.ipcMain.handle('not-electron', handler);
    const {ipcMain} = require('electron'); const indirect = wrap(ipcMain);
    indirect.handle('unknown', handler); ipcMain[computed]('unknown');
    const {ipcRenderer = fallback, ...rest} = require('electron');
    ipcRenderer.send('default'); rest.ipcRenderer.send('rest');
    fake.ipcMain.handle('name-only');)JS");
  EXPECT_TRUE(I.Electron.Boundaries.empty());
}

TEST(WebSourceOrigins, AliasBudgetAndInvalidEvidencePublishNoPartialOrigins) {
  std::string Text = "const base = require('electron');\n";
  // Reverse declaration order forces one bounded dependency traversal.
  for (unsigned I = 0; I < 80; ++I)
    Text +=
        "const a" + std::to_string(I) + " = a" + std::to_string(I + 1) + ";\n";
  Text += "const a80 = base; a0.ipcMain.handle('x', handler);";
  Inspection I(Text);
  EXPECT_EQ(I.Origins.Status, "budget_exceeded");
  EXPECT_TRUE(I.Origins.Nodes.empty());
  auto Bad = I.Modules;
  Bad.SourceID = "different";
  const auto Refused = analyzeSourceOrigins(I.Source, I.Bindings, Bad);
  EXPECT_EQ(Refused.Status, "unavailable");
  EXPECT_TRUE(Refused.Nodes.empty());
}

TEST(WebElectronSource,
     WindowPreloadRendererAndNativeWebContentsKeepExactNodes) {
  Inspection I(R"JS(const {BrowserWindow: Window} = require('electron');
    const window = new Window({webPreferences:{preload:'/private/preload.js'}});
    window.loadFile('private-index.html'); window.loadURL('https://private.invalid');
    window.webContents.send('private-channel', value);
    Window.loadFile('not-an-instance'); new Window().loadFile('other.html');)JS");
  EXPECT_EQ(I.Electron.Status, "partial");
  ASSERT_EQ(I.Electron.Boundaries.size(), 7U);
  const auto *Preload = I.find("window_preload");
  ASSERT_NE(Preload, nullptr);
  EXPECT_EQ(Preload->Text, u"/private/preload.js");
  EXPECT_EQ(I.text(Preload->ValueNode), "'/private/preload.js'");
  const auto *Renderer = I.find("renderer_file");
  ASSERT_NE(Renderer, nullptr);
  EXPECT_EQ(I.text(Renderer->ConstructionNode),
            "new Window({webPreferences:{preload:'/private/preload.js'}})");
  EXPECT_NE(I.find("renderer_url"), nullptr);
  EXPECT_NE(I.find("ipc_webcontents_send"), nullptr);
}

TEST(WebElectronSource,
     IPCAndBridgeKindsPreserveValuesWithoutExecutingCallbacks) {
  Inspection I(
      R"JS(const {ipcMain: main, ipcRenderer: renderer, contextBridge: bridge} = require('electron');
    main.handle('chan' + 'nel', (event, value) => dangerous(value));
    main.on(dynamic, handler); main.removeHandler('channel');
    renderer.invoke('channel'); renderer.sendSync('channel'); renderer.once('channel', handler);
    renderer.removeAllListeners('channel');
    bridge.exposeInMainWorld('private-api', {run: () => renderer.send('channel')});
    bridge.exposeInIsolatedWorld(1001, 'private-api', {});
    renderer?.send?.(123);)JS");
  ASSERT_EQ(I.Electron.Boundaries.size(), 11U);
  const auto *Handle = I.find("ipc_main_handle");
  ASSERT_NE(Handle, nullptr);
  EXPECT_EQ(Handle->Text, u"channel");
  EXPECT_EQ(Handle->ValueStatus, "constant_string");
  EXPECT_NE(Handle->CallbackNode, NoSourceIndex);
  EXPECT_EQ(I.find("ipc_main_listen")->ValueStatus, "dynamic_or_unavailable");
  EXPECT_EQ(I.find("bridge_isolated_world")->Text, u"private-api");
  const auto &Last = I.Electron.Boundaries.back();
  EXPECT_TRUE(Last.Optional);
  EXPECT_EQ(Last.ValueStatus, "constant_non_string");
  const auto Again =
      analyzeElectronSource(I.Source, I.Modules, I.Origins, I.Values);
  EXPECT_EQ(Again.ID, I.Electron.ID);
  EXPECT_EQ(Again.Boundaries.front().ID, Handle->ID);
}

TEST(WebElectronSource,
     OptionsWithGettersSpreadsAndDuplicatesDoNotInventPreloads) {
  Inspection I(R"JS(const {BrowserWindow} = require('electron');
    new BrowserWindow({webPreferences:{get preload(){return 'secret';}}});
    new BrowserWindow({webPreferences:{preload:'a', preload:'b'}});
    new BrowserWindow({webPreferences:{preload:'a', ...other}});
    new BrowserWindow({...other, webPreferences:{preload:'a'}});
    new BrowserWindow({webPreferences:{[dynamic]:'a', preload:'b'}});
    new BrowserWindow({webPreferences:{preload: join(base, 'preload.js')}});)JS");
  ASSERT_EQ(I.Electron.Boundaries.size(), 7U);
  const auto *Preload = I.find("window_preload");
  ASSERT_NE(Preload, nullptr);
  EXPECT_EQ(Preload->ValueStatus, "dynamic_or_unavailable");
}

TEST(WebElectronSource,
     EvidenceMismatchAndRecordBudgetDoNotPublishPartialResults) {
  Inspection Small(
      "const {ipcMain} = require('electron'); ipcMain.handle('x', handler);");
  auto Wrong = Small.Origins;
  Wrong.ModuleID = "wrong";
  auto Refused =
      analyzeElectronSource(Small.Source, Small.Modules, Wrong, Small.Values);
  EXPECT_EQ(Refused.Status, "unavailable");
  EXPECT_TRUE(Refused.Boundaries.empty());
  std::string Many = "const {ipcMain} = require('electron');\n";
  for (uint64_t I = 0; I <= MaxElectronRecords; ++I)
    Many += "ipcMain.on('x');\n";
  Inspection I(Many);
  EXPECT_EQ(I.Electron.Status, "budget_exceeded");
  EXPECT_TRUE(I.Electron.Boundaries.empty());
}
} // namespace
