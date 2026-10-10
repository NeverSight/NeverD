//===- NeverDCmdWeb.cpp - Offline web analysis commands ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline web analysis commands.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <array>

namespace neverd::cli {
namespace {
std::optional<llvm::json::Value> result(const char *Owned) {
  if (!Owned) {
    llvm::errs() << "web: allocation_failed\n";
    return std::nullopt;
  }
  auto Parsed = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    llvm::errs() << "web: invalid_backend_response\n";
    return std::nullopt;
  }
  if (!Parsed->getAsObject() ||
      Parsed->getAsObject()->getString("status") != "ok") {
    llvm::outs() << *Parsed << '\n';
    return std::nullopt;
  }
  return std::move(*Parsed);
}
} // namespace

int runWeb() {
  if (!WebArguments.empty() &&
      (WebArguments[0] == "har-preview" || WebArguments[0] == "har-import" ||
       WebArguments[0] == "interfaces" ||
       WebArguments[0] == "interface-correlate"))
    return runWebInterfaces();
  if (!WebArguments.empty() && WebArguments[0] == "integrity")
    return runWebIntegrity();
  if (!WebArguments.empty() &&
      (WebArguments[0] == "archive" || WebArguments[0] == "archive-packages" ||
       WebArguments[0] == "archive-bun"))
    return runWebArchive();
  if (!WebArguments.empty() &&
      (WebArguments[0] == "packages" || WebArguments[0] == "package-diff"))
    return runWebPackages();
  const auto Decimal = [&](size_t I, uint64_t &Value) {
    if (I >= WebArguments.size())
      return false;
    const llvm::StringRef Arg = WebArguments[I];
    return !Arg.empty() &&
           Arg.find_first_not_of("0123456789") == llvm::StringRef::npos &&
           !Arg.getAsInteger(10, Value) && std::to_string(Value) == Arg;
  };
  if (WebArguments.size() == 1 && WebArguments[0] == "capabilities") {
    auto Capabilities = result(neverd_web_capabilities_json());
    if (!Capabilities)
      return 1;
    llvm::outs() << *Capabilities << '\n';
    return 0;
  }
  const bool AsarScoped =
      WebArguments.size() >= 6 && WebArguments.size() <= 21 &&
      (WebArguments[0] == "asar-ipc" || WebArguments[0] == "asar-entries");
  const bool Scoped =
      AsarScoped || (WebArguments.size() >= 4 && WebArguments.size() <= 19 &&
                     (WebArguments[0] == "electron-ipc" ||
                      WebArguments[0] == "electron-entries"));
  const bool EntryScope = Scoped && (WebArguments[0] == "asar-entries" ||
                                     WebArguments[0] == "electron-entries");
  std::vector<std::pair<uint64_t, std::string>> SelectedSources;
  if (Scoped) {
    for (size_t I = AsarScoped ? 5 : 3; I < WebArguments.size(); ++I) {
      const auto [Index, Type] = llvm::StringRef(WebArguments[I]).split(':');
      uint64_t Value = 0;
      if (Index.empty() || Index.getAsInteger(10, Value) ||
          std::to_string(Value) != Index ||
          (Type != "script" && Type != "module" && Type != "commonjs")) {
        llvm::errs() << "web: invalid_ipc_source_selection\n";
        return 2;
      }
      SelectedSources.emplace_back(Value, Type.str());
    }
  }
  const bool AsarHTMLModules =
      WebArguments.size() == 6 && WebArguments[0] == "asar-html-modules";
  const bool HTMLModules =
      AsarHTMLModules ||
      (WebArguments.size() == 4 && WebArguments[0] == "html-modules");
  const bool AsarHTMLSource =
      AsarHTMLModules ||
      (WebArguments.size() == 6 && WebArguments[0] == "asar-html-source");
  const bool HTMLSource =
      AsarHTMLSource || HTMLModules ||
      (WebArguments.size() == 4 && WebArguments[0] == "html-source");
  const bool AsarHTML = AsarHTMLSource || (WebArguments.size() == 5 &&
                                           WebArguments[0] == "asar-html");
  const bool HTML = HTMLSource || AsarHTML ||
                    (WebArguments.size() == 3 && WebArguments[0] == "html");
  uint64_t HTMLArtifactIndex = 0, HTMLScriptIndex = 0;
  if ((HTML && !AsarHTML && !Decimal(2, HTMLArtifactIndex)) ||
      (HTMLSource && !Decimal(AsarHTML ? 5 : 3, HTMLScriptIndex))) {
    llvm::errs() << "web: invalid_html_selection\n";
    return 2;
  }
  const bool AsarView =
      WebArguments.size() == 6 && WebArguments[0] == "asar-view";
  const bool AsarElectron =
      WebArguments.size() == 6 && WebArguments[0] == "asar-electron";
  const bool AsarManifest =
      WebArguments.size() == 5 && WebArguments[0] == "asar-manifest";
  const bool AsarNavigate =
      WebArguments.size() == 6 && WebArguments[0] == "asar-navigate";
  const bool AsarAnchor =
      WebArguments.size() == 8 && WebArguments[0] == "asar-anchor";
  const bool AsarSource =
      AsarView || AsarNavigate || AsarAnchor || AsarElectron ||
      (WebArguments.size() == 6 && WebArguments[0] == "asar-source");
  const bool AsarMap =
      WebArguments.size() == 5 && WebArguments[0] == "asar-map";
  const bool AsarNative =
      WebArguments.size() == 5 && (WebArguments[0] == "asar-native" ||
                                   WebArguments[0] == "asar-native-analyze");
  const bool AsarInspect = WebArguments.size() >= 2 &&
                           WebArguments.size() <= 4 &&
                           WebArguments[0] == "asar";
  const bool Asar = AsarInspect || AsarSource || AsarMap || AsarNative ||
                    AsarManifest || AsarScoped || AsarHTML;
  const bool Manifest =
      AsarManifest || ((WebArguments.size() == 2 || WebArguments.size() == 3) &&
                       WebArguments[0] == "electron-manifest");
  uint64_t ManifestIndex = 0;
  if (((Manifest && !AsarManifest && WebArguments.size() == 3) ||
       (Scoped && !AsarScoped)) &&
      !Decimal(2, ManifestIndex)) {
    llvm::errs() << "web: invalid_manifest_selection\n";
    return 2;
  }
  uint64_t AsarArchiveIndex = 0, AsarMemberIndex = 0;
  std::optional<uint64_t> AsarDirectoryIndex;
  if (Asar) {
    uint64_t Directory = 0;
    if ((WebArguments.size() > 2 && !Decimal(2, AsarArchiveIndex)) ||
        (WebArguments.size() > 3 && WebArguments[3] != "-" &&
         !Decimal(3, Directory)) ||
        (!AsarInspect && !Decimal(4, AsarMemberIndex))) {
      llvm::errs() << "web: invalid_asar_selection\n";
      return 2;
    }
    if (WebArguments.size() > 3 && WebArguments[3] != "-")
      AsarDirectoryIndex = Directory;
  }
  const bool Bindings =
      WebArguments.size() == 3 && WebArguments[0] == "bindings";
  const bool Semantics =
      WebArguments.size() == 3 && WebArguments[0] == "semantics";
  const bool Modules =
      HTMLModules || (WebArguments.size() == 3 && WebArguments[0] == "modules");
  const bool Bundles = WebArguments.size() == 3 && WebArguments[0] == "bundles";
  const bool BunView =
      WebArguments.size() == 4 && WebArguments[0] == "bun-view";
  const bool BunElectron =
      WebArguments.size() == 4 && WebArguments[0] == "bun-electron";
  const bool Electron =
      BunElectron || AsarElectron ||
      (WebArguments.size() == 3 && WebArguments[0] == "electron-source");
  const bool BunNavigate =
      WebArguments.size() == 4 && WebArguments[0] == "bun-navigate";
  const bool BunAnchor =
      WebArguments.size() == 6 && WebArguments[0] == "bun-anchor";
  const bool BunNative =
      WebArguments.size() == 3 && (WebArguments[0] == "bun-native" ||
                                   WebArguments[0] == "bun-native-analyze");
  const bool Native =
      BunNative || AsarNative ||
      (WebArguments.size() == 2 &&
       (WebArguments[0] == "native" || WebArguments[0] == "native-analyze"));
  const bool NativeAnalyze =
      Native && (WebArguments[0] == "native-analyze" ||
                 WebArguments[0] == "bun-native-analyze" ||
                 WebArguments[0] == "asar-native-analyze");
  const bool BunSource = BunView || BunNavigate || BunAnchor || BunElectron;
  const bool View = BunView || AsarView ||
                    (WebArguments.size() == 3 && WebArguments[0] == "view");
  const bool Navigate =
      BunNavigate || AsarNavigate ||
      (WebArguments.size() == 3 && WebArguments[0] == "navigate");
  const bool Anchor = BunAnchor || AsarAnchor ||
                      (WebArguments.size() == 5 && WebArguments[0] == "anchor");
  uint64_t AnchorOffset = 0, AnchorLength = 0;
  if (Anchor) {
    if (!Decimal(AsarAnchor  ? 6
                 : BunAnchor ? 4
                             : 3,
                 AnchorOffset) ||
        !Decimal(AsarAnchor  ? 7
                 : BunAnchor ? 5
                             : 4,
                 AnchorLength)) {
      llvm::errs() << "web: invalid_source_range\n";
      return 2;
    }
  }
  const bool Source = Bindings || Semantics || Modules || Bundles || View ||
                      Navigate || Anchor || AsarSource || Electron ||
                      HTMLSource ||
                      (WebArguments.size() == 3 && WebArguments[0] == "source");
  const bool BunMap = WebArguments.size() == 3 && WebArguments[0] == "bun-map";
  const bool BunExport =
      WebArguments.size() == 3 && WebArguments[0] == "bun-export";
  const bool Map = BunMap || AsarMap ||
                   (WebArguments.size() == 2 && WebArguments[0] == "map");
  const bool Bun = BunMap || BunSource || BunNative || BunExport ||
                   (WebArguments.size() == 2 && WebArguments[0] == "bun");
  uint64_t BunModule = 0;
  if ((BunMap || BunSource || BunNative) &&
      (llvm::StringRef(WebArguments[2]).empty() ||
       llvm::StringRef(WebArguments[2]).find_first_not_of("0123456789") !=
           llvm::StringRef::npos ||
       llvm::StringRef(WebArguments[2]).getAsInteger(10, BunModule))) {
    llvm::errs() << "web: invalid_module_index\n";
    return 2;
  }
  if (!Source && !Map && !Bun && !Native && !Asar && !Manifest && !Scoped &&
      !HTML && (WebArguments.size() != 2 || WebArguments[0] != "inspect")) {
    llvm::errs()
        << "usage: neverd web capabilities | inspect <file-or-directory> | "
           "html <root> <artifact-index> | html-source|html-modules <root> "
           "<artifact-index> "
           "<script-index> | "
           "asar-html <root> <archive-index> <unpacked-index|-> <member-index> "
           "| "
           "asar-html-source|asar-html-modules <root> <archive-index> "
           "<unpacked-index|-> "
           "<member-index> <script-index> | "
           "electron-ipc|electron-entries <root> <manifest-index> "
           "<source-index:source-type>... "
           "| "
           "asar-ipc|asar-entries <root> <archive-index> <unpacked-index|-> "
           "<manifest-member-index> "
           "<source-member-index:source-type>... | "
           "source|bindings|semantics|modules|bundles|view|navigate <file> "
           "<script|module|commonjs> "
           "| map|bun "
           "<file> | bun-export <file> <new-output-directory> | "
           "bun-map <file> <module-index> | "
           "anchor <file> <script|module|commonjs> <byte-offset> "
           "<byte-length> | bun-view|bun-navigate <file> <module-index> "
           "<source-type> | "
           "bun-anchor <file> <module-index> <source-type> <byte-offset> "
           "<byte-length> | native|native-analyze <file> | "
           "bun-native|bun-native-analyze <file> <module-index> | "
           "asar <file-or-root> [archive-index [unpacked-directory-index|-]] | "
           "asar-source|asar-view|asar-navigate <root> <archive-index> "
           "<unpacked-directory-index|-> <member-index> <source-type> | "
           "asar-anchor <root> <archive-index> <unpacked-directory-index|-> "
           "<member-index> <source-type> <byte-offset> <byte-length> | "
           "asar-map|asar-native|asar-native-analyze <root> <archive-index> "
           "<unpacked-directory-index|-> <member-index> | "
           "electron-manifest <file-or-root> [manifest-index] | "
           "electron-source <file> <source-type> | "
           "asar-manifest <root> <archive-index> <unpacked-directory-index|-> "
           "<member-index> | "
           "asar-electron <root> <archive-index> <unpacked-directory-index|-> "
           "<member-index> <source-type> | "
           "bun-electron <file> <module-index> <source-type>\n";
    return 2;
  }
  struct Guard {
    neverd_web_session_t Handle = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(Handle); }
  } Session;
  if (!Session.Handle) {
    llvm::errs() << "web: capability_unavailable\n";
    return 1;
  }
  const auto &Path = WebArguments[1];
  auto Preview = result(neverd_web_import_preview_json(
      Session.Handle, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  auto Token = Preview->getAsObject()->getString("preview_token");
  if (!Token)
    return 1;
  auto Metadata = result(neverd_web_import_commit_json(
      Session.Handle, Token->data(), Token->size()));
  if (!Metadata)
    return 1;
  // Inspection publishes only the previewed metadata-only policy. Raw names,
  // source/config and observation content never cross this CLI operation.
  llvm::outs() << *Metadata << '\n';
  const auto Revision = Metadata->getAsObject()->getString("revision");
  if (!Revision)
    return 1;
  if (Source || Map || Bun || Native || Asar || Manifest || Scoped || HTML) {
    auto Root = result(neverd_web_artifacts_json(
        Session.Handle, Revision->data(), Revision->size(),
        Asar                 ? AsarArchiveIndex
        : HTML               ? HTMLArtifactIndex
        : Manifest || Scoped ? ManifestIndex
                             : 0,
        1));
    if (!Root)
      return 1;
    const auto *Items = Root->getAsObject()->getArray("items");
    if (!Items || Items->size() != 1 || !(*Items)[0].getAsObject())
      return 1;
    const auto ID = (*Items)[0].getAsObject()->getString("artifact_id");
    if (!ID)
      return 1;
    std::string MapArtifactID = ID->str(), SourceArtifactID = ID->str();
    std::string NativeSelectionID = ID->str();
    std::string AsarExtractionID;
    if (Asar) {
      std::string DirectoryID;
      if (AsarDirectoryIndex) {
        auto Directory = result(neverd_web_artifacts_json(
            Session.Handle, Revision->data(), Revision->size(),
            *AsarDirectoryIndex, 1));
        const auto *Entries =
            Directory ? Directory->getAsObject()->getArray("items") : nullptr;
        if (!Entries || Entries->size() != 1)
          return 1;
        const auto Selected =
            (*Entries)[0].getAsObject()->getString("artifact_id");
        if (!Selected)
          return 1;
        DirectoryID = Selected->str();
      }
      auto Summary = result(neverd_web_asar_extract_json(
          Session.Handle, Revision->data(), Revision->size(), ID->data(),
          ID->size(), DirectoryID.data(), DirectoryID.size()));
      if (!Summary)
        return 1;
      llvm::outs() << *Summary << '\n';
      const auto Extraction =
          Summary->getAsObject()->getString("extraction_id");
      if (!Extraction)
        return 1;
      AsarExtractionID = Extraction->str();
      uint64_t Offset = AsarInspect ? 0 : AsarMemberIndex;
      do {
        auto Page = result(neverd_web_asar_records_json(
            Session.Handle, Revision->data(), Revision->size(),
            Extraction->data(), Extraction->size(), Offset,
            AsarInspect ? 128 : 1));
        if (!Page)
          return 1;
        llvm::outs() << *Page << '\n';
        if (!AsarInspect) {
          const auto *Members = Page->getAsObject()->getArray("items");
          if (!Members || Members->size() != 1 ||
              (*Members)[0].getAsObject()->getString("availability") !=
                  "available") {
            llvm::errs() << "web: artifact_bytes_unavailable\n";
            return 1;
          }
          const auto Selected =
              (*Members)[0].getAsObject()->getString("member_id");
          if (!Selected)
            return 1;
          MapArtifactID = SourceArtifactID = NativeSelectionID =
              Selected->str();
          break;
        }
        const auto Next = Page->getAsObject()->getInteger("next_offset");
        if (!Next)
          return 0;
        Offset = *Next;
      } while (true);
    }
    if (Bun) {
      auto Summary = result(neverd_web_bun_extract_json(
          Session.Handle, Revision->data(), Revision->size(), ID->data(),
          ID->size()));
      if (!Summary)
        return 1;
      llvm::outs() << *Summary << '\n';
      const auto Extraction =
          Summary->getAsObject()->getString("extraction_id");
      if (!Extraction)
        return 1;
      if (BunExport) {
        const auto &Directory = WebArguments[2];
        auto Export = result(neverd_web_bun_export_json(
            Session.Handle, Revision->data(), Revision->size(),
            Extraction->data(), Extraction->size(), Directory.data(),
            Directory.size()));
        if (!Export)
          return 1;
        llvm::outs() << *Export << '\n';
        return 0;
      }
      if (BunMap || BunSource || BunNative) {
        auto Module = result(neverd_web_bun_records_json(
            Session.Handle, Revision->data(), Revision->size(),
            Extraction->data(), Extraction->size(), "modules", 7, BunModule,
            1));
        if (!Module)
          return 1;
        const auto *Items = Module->getAsObject()->getArray("items");
        const auto RegionID = Items && Items->size() == 1
                                  ? (*Items)[0].getAsObject()->getString(
                                        BunMap      ? "source_map_region_id"
                                        : BunNative ? "content_region_id"
                                                    : "source_artifact_id")
                                  : std::nullopt;
        if (!RegionID) {
          llvm::errs() << (BunMap      ? "web: bun_source_map_not_supplied\n"
                           : BunNative ? "web: bun_native_asset_unavailable\n"
                                       : "web: bun_source_unavailable\n");
          return 1;
        }
        if (BunNative)
          NativeSelectionID = RegionID->str();
        else if (BunMap)
          MapArtifactID = RegionID->str();
        else
          SourceArtifactID = RegionID->str();
        llvm::outs() << *Module << '\n';
      } else
        for (llvm::StringRef Kind : {"modules", "regions"}) {
          uint64_t Offset = 0;
          do {
            auto Page = result(neverd_web_bun_records_json(
                Session.Handle, Revision->data(), Revision->size(),
                Extraction->data(), Extraction->size(), Kind.data(),
                Kind.size(), Offset, 128));
            if (!Page)
              return 1;
            llvm::outs() << *Page << '\n';
            const auto Next = Page->getAsObject()->getInteger("next_offset");
            if (!Next)
              break;
            Offset = *Next;
          } while (true);
        }
      if (!BunMap && !BunSource && !BunNative)
        return 0;
    }
    if (Manifest || Scoped) {
      auto Analysis = result(neverd_web_electron_manifest_analyze_json(
          Session.Handle, Revision->data(), Revision->size(),
          SourceArtifactID.data(), SourceArtifactID.size()));
      if (!Analysis)
        return 1;
      llvm::outs() << *Analysis << '\n';
      if (!Scoped)
        return 0;
      llvm::json::Array SourceIDs;
      for (const auto &[Index, Type] : SelectedSources) {
        auto Page = result(
            AsarScoped
                ? neverd_web_asar_records_json(
                      Session.Handle, Revision->data(), Revision->size(),
                      AsarExtractionID.data(), AsarExtractionID.size(), Index,
                      1)
                : neverd_web_artifacts_json(Session.Handle, Revision->data(),
                                            Revision->size(), Index, 1));
        const auto *Entries =
            Page ? Page->getAsObject()->getArray("items") : nullptr;
        const auto *Entry = Entries && Entries->size() == 1
                                ? (*Entries)[0].getAsObject()
                                : nullptr;
        const auto Selected =
            Entry ? Entry->getString(AsarScoped ? "member_id" : "artifact_id")
                  : std::nullopt;
        if (!Selected)
          return 1;
        auto SourceSummary = result(neverd_web_source_analyze_json(
            Session.Handle, Revision->data(), Revision->size(),
            Selected->data(), Selected->size(), Type.data(), Type.size()));
        if (!SourceSummary)
          return 1;
        llvm::outs() << *SourceSummary << '\n';
        const auto SourceID =
            SourceSummary->getAsObject()->getString("source_id");
        if (!SourceID ||
            SourceSummary->getAsObject()->getString("parse_status") != "parsed")
          return 1;
        auto Evidence = result(neverd_web_electron_source_analyze_json(
            Session.Handle, Revision->data(), Revision->size(),
            SourceID->data(), SourceID->size()));
        if (!Evidence)
          return 1;
        llvm::outs() << *Evidence << '\n';
        // SourceSummary is destroyed each iteration; JSON StringRef values
        // borrow storage, so the aggregate request must own each source ID.
        SourceIDs.emplace_back(SourceID->str());
      }
      std::string Options;
      llvm::raw_string_ostream(Options) << llvm::json::Value(llvm::json::Object{
          {"schema_version", 1}, {"source_ids", std::move(SourceIDs)}});
      const auto Analyze = EntryScope ? neverd_web_electron_entries_analyze_json
                                      : neverd_web_electron_ipc_analyze_json;
      const auto Records = EntryScope ? neverd_web_electron_entry_records_json
                                      : neverd_web_electron_ipc_records_json;
      auto Correlation =
          result(Analyze(Session.Handle, Revision->data(), Revision->size(),
                         SourceArtifactID.data(), SourceArtifactID.size(),
                         Options.data(), Options.size()));
      if (!Correlation)
        return 1;
      llvm::outs() << *Correlation << '\n';
      const auto AnalysisID = Correlation->getAsObject()->getString(
          EntryScope ? "electron_entries_id" : "electron_ipc_id");
      if (!AnalysisID)
        return 1;
      const std::vector<llvm::StringRef> Kinds =
          EntryScope ? std::vector<llvm::StringRef>{"sources", "entries"}
                     : std::vector<llvm::StringRef>{"sources", "channels",
                                                    "endpoints"};
      for (const llvm::StringRef Kind : Kinds) {
        uint64_t Offset = 0;
        do {
          auto Page =
              result(Records(Session.Handle, Revision->data(), Revision->size(),
                             AnalysisID->data(), AnalysisID->size(),
                             Kind.data(), Kind.size(), Offset, 128));
          if (!Page)
            return 1;
          llvm::outs() << *Page << '\n';
          const auto Next = Page->getAsObject()->getInteger("next_offset");
          if (!Next)
            break;
          Offset = *Next;
        } while (true);
      }
      return 0;
    }
    if (Native) {
      struct NativeGuard {
        neverd_session_t Handle = nullptr;
        ~NativeGuard() { neverd_session_destroy(Handle); }
      } NativeSession;
      auto Opened = result(neverd_web_native_open_json(
          Session.Handle, Revision->data(), Revision->size(),
          NativeSelectionID.data(), NativeSelectionID.size(),
          &NativeSession.Handle));
      if (!Opened)
        return 1;
      if (NativeAnalyze)
        Opened = result(neverd_web_native_analyze_json(NativeSession.Handle));
      if (!Opened)
        return 1;
      llvm::outs() << *Opened << '\n';
      return 0;
    }
    if (Map) {
      auto Summary = result(neverd_web_source_map_analyze_json(
          Session.Handle, Revision->data(), Revision->size(),
          MapArtifactID.data(), MapArtifactID.size()));
      if (!Summary)
        return 1;
      llvm::outs() << *Summary << '\n';
      const auto MapID = Summary->getAsObject()->getString("map_id");
      if (!MapID)
        return 1;
      for (const auto Query : {neverd_web_source_map_sources_json,
                               neverd_web_source_map_segments_json}) {
        uint64_t Offset = 0;
        do {
          auto Page =
              result(Query(Session.Handle, Revision->data(), Revision->size(),
                           MapID->data(), MapID->size(), Offset, 128));
          if (!Page)
            return 1;
          llvm::outs() << *Page << '\n';
          const auto Next = Page->getAsObject()->getInteger("next_offset");
          if (!Next)
            break;
          Offset = *Next;
        } while (true);
      }
      return 0;
    }
    std::string HTMLType;
    if (HTML) {
      auto Analysis = result(neverd_web_html_analyze_json(
          Session.Handle, Revision->data(), Revision->size(),
          SourceArtifactID.data(), SourceArtifactID.size()));
      if (!Analysis)
        return 1;
      llvm::outs() << *Analysis << '\n';
      const auto HTMLID = Analysis->getAsObject()->getString("html_id");
      if (!HTMLID)
        return 1;
      for (const llvm::StringRef Kind : {"scripts", "bases", "import_maps"}) {
        if (HTMLSource && Kind != "scripts")
          continue;
        uint64_t Offset = HTMLSource ? HTMLScriptIndex : 0;
        do {
          auto Page = result(neverd_web_html_records_json(
              Session.Handle, Revision->data(), Revision->size(),
              HTMLID->data(), HTMLID->size(), Kind.data(), Kind.size(), Offset,
              HTMLSource ? 1 : 128));
          if (!Page)
            return 1;
          llvm::outs() << *Page << '\n';
          if (HTMLSource) {
            const auto *Items = Page->getAsObject()->getArray("items");
            const auto *Selected = Items && Items->size() == 1
                                       ? (*Items)[0].getAsObject()
                                       : nullptr;
            const auto Candidate =
                Selected ? Selected->getString("candidate_artifact_id")
                         : std::nullopt;
            const auto Type =
                Selected ? Selected->getString("source_type") : std::nullopt;
            if (!Candidate || !Type) {
              llvm::errs() << "web: html_source_unavailable\n";
              return 1;
            }
            SourceArtifactID = Candidate->str();
            HTMLType = Type->str();
            break;
          }
          const auto Next = Page->getAsObject()->getInteger("next_offset");
          if (!Next)
            break;
          Offset = *Next;
        } while (true);
      }
      if (!HTMLSource)
        return Analysis->getAsObject()->getString("analysis_status") ==
                           "partial" &&
                       Analysis->getAsObject()->getString("link_status") ==
                           "partial" &&
                       Analysis->getAsObject()->getString(
                           "import_map_analysis") == "partial"
                   ? 0
                   : 1;
    }
    const auto &Type = HTMLSource ? HTMLType
                                  : WebArguments[AsarSource  ? 5
                                                 : BunSource ? 3
                                                             : 2];
    auto Summary = result(neverd_web_source_analyze_json(
        Session.Handle, Revision->data(), Revision->size(),
        SourceArtifactID.data(), SourceArtifactID.size(), Type.data(),
        Type.size()));
    if (!Summary)
      return 1;
    llvm::outs() << *Summary << '\n';
    if (Summary->getAsObject()->getString("parse_status") != "parsed")
      return 1;
    const auto SourceID = Summary->getAsObject()->getString("source_id");
    if (!SourceID)
      return 1;
    if (View || Anchor) {
      // CLI publication selects only the default structural policy. Revealing
      // original regions requires the explicit local SDK review workflow.
      auto Preview = result(neverd_web_source_view_preview_json(
          Session.Handle, Revision->data(), Revision->size(), SourceID->data(),
          SourceID->size(), nullptr, 0));
      if (!Preview)
        return 1;
      llvm::outs() << *Preview << '\n';
      const auto Token = Preview->getAsObject()->getString("preview_token");
      if (!Token)
        return 1;
      auto Committed = result(neverd_web_source_view_commit_json(
          Session.Handle, Revision->data(), Revision->size(), Token->data(),
          Token->size()));
      if (!Committed)
        return 1;
      llvm::outs() << *Committed << '\n';
      const auto ViewID = Committed->getAsObject()->getString("view_id");
      if (!ViewID)
        return 1;
      if (Anchor) {
        auto Location = result(neverd_web_source_anchor_json(
            Session.Handle, Revision->data(), Revision->size(),
            SourceID->data(), SourceID->size(), AnchorOffset, AnchorLength,
            ViewID->data(), ViewID->size()));
        if (!Location)
          return 1;
        llvm::outs() << *Location << '\n';
        const auto *Projected = Location->getAsObject()->getObject("view");
        if (!Projected)
          return 1;
        const auto Start = Projected->getString("byte_offset"),
                   Length = Projected->getString("byte_length");
        uint64_t At = 0, Size = 0;
        if (!Start || !Length || Start->getAsInteger(10, At) ||
            Length->getAsInteger(10, Size))
          return 1;
        if (Size) {
          auto Chunk = result(neverd_web_source_view_chunk_json(
              Session.Handle, Revision->data(), Revision->size(),
              ViewID->data(), ViewID->size(), At,
              std::min<uint64_t>(Size, 65536)));
          if (!Chunk)
            return 1;
          llvm::outs() << *Chunk << '\n';
        }
        return 0;
      }
      uint64_t Offset = 0;
      do {
        auto Page = result(neverd_web_source_view_records_json(
            Session.Handle, Revision->data(), Revision->size(), ViewID->data(),
            ViewID->size(), Offset, 128));
        if (!Page)
          return 1;
        llvm::outs() << *Page << '\n';
        const auto Next = Page->getAsObject()->getInteger("next_offset");
        if (!Next)
          break;
        Offset = *Next;
      } while (true);
      Offset = 0;
      do {
        auto Chunk = result(neverd_web_source_view_chunk_json(
            Session.Handle, Revision->data(), Revision->size(), ViewID->data(),
            ViewID->size(), Offset, 65536));
        if (!Chunk)
          return 1;
        llvm::outs() << *Chunk << '\n';
        const auto Next = Chunk->getAsObject()->getString("next_byte_offset");
        if (!Next)
          break;
        if (Next->getAsInteger(10, Offset))
          return 1;
      } while (true);
      return 0;
    }
    if (Electron) {
      auto Analysis = result(neverd_web_electron_source_analyze_json(
          Session.Handle, Revision->data(), Revision->size(), SourceID->data(),
          SourceID->size()));
      if (!Analysis)
        return 1;
      llvm::outs() << *Analysis << '\n';
      if (Analysis->getAsObject()->getString("analysis_status") != "partial")
        return 1;
      uint64_t Offset = 0;
      do {
        auto Page = result(neverd_web_electron_source_records_json(
            Session.Handle, Revision->data(), Revision->size(),
            SourceID->data(), SourceID->size(), Offset, 128));
        if (!Page)
          return 1;
        llvm::outs() << *Page << '\n';
        const auto Next = Page->getAsObject()->getInteger("next_offset");
        if (!Next)
          break;
        Offset = *Next;
      } while (true);
      return 0;
    }
    if (Navigate) {
      auto Analysis = result(neverd_web_source_navigation_analyze_json(
          Session.Handle, Revision->data(), Revision->size(), SourceID->data(),
          SourceID->size()));
      if (!Analysis)
        return 1;
      llvm::outs() << *Analysis << '\n';
      const auto Status =
          Analysis->getAsObject()->getString("navigation_status");
      if (Status != "ok" && Status != "partial")
        return 1;
      for (llvm::StringRef Kind : {"functions", "calls", "references"}) {
        uint64_t Offset = 0;
        do {
          auto Page = result(neverd_web_source_navigation_records_json(
              Session.Handle, Revision->data(), Revision->size(),
              SourceID->data(), SourceID->size(), Kind.data(), Kind.size(),
              Offset, 128));
          if (!Page)
            return 1;
          llvm::outs() << *Page << '\n';
          const auto Next = Page->getAsObject()->getInteger("next_offset");
          if (!Next)
            break;
          Offset = *Next;
        } while (true);
      }
      return 0;
    }
    if (Semantics) {
      auto Analysis = result(neverd_web_source_semantics_analyze_json(
          Session.Handle, Revision->data(), Revision->size(), SourceID->data(),
          SourceID->size()));
      if (!Analysis)
        return 1;
      llvm::outs() << *Analysis << '\n';
      for (const auto Kind : {"value_status", "effect_status"}) {
        const auto Status = Analysis->getAsObject()->getString(Kind);
        if (Status != "ok" && Status != "partial")
          return 1;
      }
      uint64_t Offset = 0;
      do {
        auto Page = result(neverd_web_source_semantic_records_json(
            Session.Handle, Revision->data(), Revision->size(),
            SourceID->data(), SourceID->size(), Offset, 128));
        if (!Page)
          return 1;
        llvm::outs() << *Page << '\n';
        const auto Next = Page->getAsObject()->getInteger("next_offset");
        if (!Next)
          break;
        Offset = *Next;
      } while (true);
      return 0;
    }
    if (Bindings || Modules || Bundles) {
      const auto Analyze = Bundles   ? neverd_web_source_bundles_analyze_json
                           : Modules ? neverd_web_source_modules_analyze_json
                                     : neverd_web_source_bindings_analyze_json;
      const auto Records = Bundles   ? neverd_web_source_bundle_records_json
                           : Modules ? neverd_web_source_module_records_json
                                     : neverd_web_source_binding_records_json;
      auto Analysis =
          result(Analyze(Session.Handle, Revision->data(), Revision->size(),
                         SourceID->data(), SourceID->size()));
      if (!Analysis)
        return 1;
      llvm::outs() << *Analysis << '\n';
      const auto Status =
          Analysis->getAsObject()->getString(Bundles   ? "bundle_status"
                                             : Modules ? "module_status"
                                                       : "binding_status");
      if (Status != "ok" && Status != "partial" &&
          !(Bundles && Status == "not_detected"))
        return 1;
      if (Modules) {
        const auto LinkStatus =
            Analysis->getAsObject()->getString("link_status");
        if (LinkStatus != "ok" && LinkStatus != "partial")
          return 1;
      }
      const std::array<llvm::StringRef, 4> Kinds =
          Bundles   ? std::array<llvm::StringRef, 4>{"bundles", "modules",
                                                     "dependencies", "regions"}
          : Modules ? std::array<llvm::StringRef, 4>{"requests", "imports",
                                                     "exports", "attributes"}
                    : std::array<llvm::StringRef, 4>{
                          "scopes", "bindings", "declarations", "references"};
      for (llvm::StringRef Kind : Kinds) {
        uint64_t Offset = 0;
        do {
          auto Page =
              result(Records(Session.Handle, Revision->data(), Revision->size(),
                             SourceID->data(), SourceID->size(), Kind.data(),
                             Kind.size(), Offset, 128));
          if (!Page)
            return 1;
          llvm::outs() << *Page << '\n';
          const auto Next = Page->getAsObject()->getInteger("next_offset");
          if (!Next)
            break;
          Offset = *Next;
        } while (true);
      }
      return 0;
    }
    uint64_t Offset = 0;
    do {
      auto Page = result(neverd_web_source_nodes_json(
          Session.Handle, Revision->data(), Revision->size(), SourceID->data(),
          SourceID->size(), Offset, 128));
      if (!Page)
        return 1;
      llvm::outs() << *Page << '\n';
      auto Next = Page->getAsObject()->getInteger("next_offset");
      if (!Next)
        break;
      Offset = *Next;
    } while (true);
    return 0;
  }
  uint64_t Offset = 0;
  do {
    auto Page = result(neverd_web_artifacts_json(
        Session.Handle, Revision->data(), Revision->size(), Offset, 128));
    if (!Page)
      return 1;
    llvm::outs() << *Page << '\n';
    auto Next = Page->getAsObject()->getInteger("next_offset");
    if (!Next)
      break;
    Offset = *Next;
  } while (true);
  return 0;
}
} // namespace neverd::cli
