#include "SourceModel.h"

#include "neverd/web/Electron.h"
#include "neverd/web/Session.h"

#include <span>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
struct Inspector {
  const SourceAnalysis &S;
  const SourceModuleAnalysis &M;
  const SourceOrigins &O;
  const SourceValueAnalysis &V;
  ElectronSource A;
  void step(uint64_t Count = 1) {
    if (Count > MaxElectronSteps - A.Steps)
      throw Error("electron_source_budget_exceeded");
    A.Steps += Count;
  }
  uint32_t child(uint32_t I, std::string_view Field, uint32_t Ordinal = 0) {
    if (I == None)
      return None;
    for (const auto &C : S.Nodes.at(I).Children) {
      step();
      if (C.Field == Field && C.Ordinal == Ordinal)
        return C.Index;
    }
    return None;
  }
  bool is(uint32_t I, std::string_view Kind) const {
    return I != None && S.Nodes.at(I).Kind == Kind;
  }
  // Object options are syntax evidence. Duplicate/computed/spread properties
  // prevent selecting a unique value; getters never become literal values.
  uint32_t property(uint32_t I, std::u16string_view Name) {
    if (!is(I, "ObjectExpression"))
      return None;
    uint32_t Found = None;
    for (const auto &C : S.Nodes[I].Children) {
      step();
      if (C.Field != "properties" || !is(C.Index, "Property"))
        return None;
      const auto &P = S.Nodes[C.Index];
      const auto Key = child(C.Index, "key");
      if (Key == None)
        return None;
      const auto &K = S.Nodes[Key];
      const auto *Text = K.text(
          K.Kind == "Identifier" && !P.flag("computed") ? "name" : "value");
      if (!Text || (P.flag("computed") && K.Kind != "StringLiteral"))
        return None;
      if (*Text != Name)
        continue;
      if (Found != None || !P.text("kind") || *P.text("kind") != u"init" ||
          P.flag("method"))
        return None;
      Found = child(C.Index, "value");
    }
    return Found;
  }
  void add(uint32_t I, const SourceOrigin &Origin, std::string Kind,
           uint32_t Value = None, uint32_t Callback = None) {
    step();
    if (A.Boundaries.size() >= MaxElectronRecords)
      throw Error("electron_source_budget_exceeded");
    ElectronBoundary B;
    B.ID = identity("electron-boundary", {A.ID, S.Nodes[I].ID, Kind});
    B.Kind = std::move(Kind);
    B.Node = I;
    B.Request = Origin.Request;
    B.OriginEvidence = Origin.Evidence;
    B.ValueNode = Value;
    B.CallbackNode = Callback;
    B.ConstructionNode =
        S.Nodes[I].Kind == "NewExpression" ? I : Origin.Construction;
    B.Optional = S.Nodes[I].Kind == "OptionalCallExpression" ||
                 S.Nodes[I].flag("optional");
    if (Value != None) {
      B.ValueStatus = "dynamic_or_unavailable";
      if (V.Nodes.size() == S.Nodes.size()) {
        const auto &Constant = V.Nodes[Value];
        if (Constant.Status == "constant" && Constant.Value &&
            Constant.Value->Kind == PrimitiveKind::String) {
          const auto &Text = Constant.Value->String;
          if (Text.size() > MaxSourceOriginUnits - A.StringUnits)
            throw Error("electron_source_budget_exceeded");
          A.StringUnits += Text.size();
          B.Text = Text;
          B.ValueStatus = "constant_string";
        } else if (Constant.Status == "constant") {
          B.ValueStatus = "constant_non_string";
        }
      }
    }
    A.Boundaries.push_back(std::move(B));
  }
  void run() {
    step(validateSourceModel(S));
    if (M.SourceID != S.ID || O.SourceID != S.ID || V.SourceID != S.ID ||
        O.ModuleID != M.ID || O.BindingID != M.BindingID)
      throw Error("electron_source_evidence_mismatch");
    if (O.Status != "partial" || O.Nodes.size() != S.Nodes.size())
      throw Error("electron_source_origins_unavailable");
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      step();
      const auto &N = S.Nodes[I];
      if (N.Kind != "CallExpression" && N.Kind != "OptionalCallExpression" &&
          N.Kind != "NewExpression")
        continue;
      const auto Callee = child(I, "callee");
      if (Callee == None || !O.Nodes[Callee])
        continue;
      const auto &Origin = *O.Nodes[Callee];
      if (Origin.Request >= M.Requests.size() ||
          (Origin.Construction != None &&
           Origin.Construction >= S.Nodes.size()))
        throw Error("invalid_source_origin_model");
      const auto &R = M.Requests[Origin.Request];
      if (R.Specifier >= M.Names.size())
        throw Error("invalid_source_module_model");
      const auto &Specifier = M.Names[R.Specifier];
      if (Specifier != u"electron" && Specifier != u"electron/main" &&
          Specifier != u"electron/renderer")
        continue;
      auto Path = std::span<const std::u16string>(Origin.Members);
      if (!Path.empty() && Path.front() == u"default")
        Path = Path.subspan(1);
      const auto Value = child(I, "arguments"),
                 Callback = child(I, "arguments", 1);
      if (N.Kind == "NewExpression" && Path.size() == 1 &&
          Path[0] == u"BrowserWindow" && Origin.Construction == None) {
        add(I, Origin, "window_construct", Value);
        const auto Preferences = property(Value, u"webPreferences");
        const auto Preload = property(Preferences, u"preload");
        if (Preload != None)
          add(I, Origin, "window_preload", Preload);
        continue;
      }
      if (N.Kind == "NewExpression")
        continue;
      if (Origin.Construction != None && Path.size() == 2 &&
          Path[0] == u"BrowserWindow") {
        if (Path[1] == u"loadFile")
          add(I, Origin, "renderer_file", Value);
        else if (Path[1] == u"loadURL")
          add(I, Origin, "renderer_url", Value);
        continue;
      }
      if (Origin.Construction != None && Path.size() == 3 &&
          Path[0] == u"BrowserWindow" && Path[1] == u"webContents") {
        if (Path[2] == u"send" || Path[2] == u"postMessage")
          add(I, Origin, "ipc_webcontents_send", Value);
        continue;
      }
      if (Origin.Construction != None || Path.size() != 2)
        continue;
      if (Path[0] == u"ipcMain") {
        if (Path[1] == u"handle" || Path[1] == u"handleOnce")
          add(I, Origin, "ipc_main_handle", Value, Callback);
        else if (Path[1] == u"on" || Path[1] == u"once")
          add(I, Origin, "ipc_main_listen", Value, Callback);
        else if (Path[1] == u"removeHandler" || Path[1] == u"removeListener" ||
                 Path[1] == u"removeAllListeners")
          add(I, Origin, "ipc_main_remove", Value);
      } else if (Path[0] == u"ipcRenderer") {
        if (Path[1] == u"invoke")
          add(I, Origin, "ipc_renderer_invoke", Value);
        else if (Path[1] == u"send" || Path[1] == u"sendSync" ||
                 Path[1] == u"postMessage")
          add(I, Origin, "ipc_renderer_send", Value);
        else if (Path[1] == u"on" || Path[1] == u"once")
          add(I, Origin, "ipc_renderer_listen", Value, Callback);
        else if (Path[1] == u"removeListener" ||
                 Path[1] == u"removeAllListeners")
          add(I, Origin, "ipc_renderer_remove", Value);
      } else if (Path[0] == u"contextBridge") {
        if (Path[1] == u"exposeInMainWorld")
          add(I, Origin, "bridge_main_world", Value, Callback);
        else if (Path[1] == u"exposeInIsolatedWorld")
          add(I, Origin, "bridge_isolated_world", Callback,
              child(I, "arguments", 2));
      }
    }
    A.Status = "partial";
    A.Reason = "source_candidates_not_runtime_boundaries";
  }
};
} // namespace

ElectronSource analyzeElectronSource(const SourceAnalysis &S,
                                     const SourceModuleAnalysis &M,
                                     const SourceOrigins &O,
                                     const SourceValueAnalysis &V) {
  Inspector I{S, M, O, V};
  I.A.SourceID = S.ID;
  I.A.BindingID = M.BindingID;
  I.A.ModuleID = M.ID;
  I.A.OriginID = O.ID;
  I.A.ValueID = V.ID;
  I.A.OriginStatus = O.Status;
  I.A.OriginReason = O.Reason;
  I.A.ValueStatus = V.Status;
  I.A.ID = identity("electron-source",
                    {S.ID, M.ID, O.ID, V.ID, ElectronSourceProfile});
  try {
    I.run();
  } catch (const Error &E) {
    I.A.Boundaries.clear();
    I.A.Reason = E.what();
    I.A.Status = I.A.Reason == "electron_source_budget_exceeded"
                     ? "budget_exceeded"
                     : "unavailable";
  }
  return std::move(I.A);
}
} // namespace neverd::web
