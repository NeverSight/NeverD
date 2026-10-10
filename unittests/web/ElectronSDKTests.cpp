#include "AsarFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using namespace neverd::web::test;
using llvm::json::Value;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("null C ABI result");
  const std::string Bytes(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Bytes.find("CANARY"), std::string::npos);
  auto Parsed = llvm::json::parse(Bytes);
  if (!Parsed)
    throw std::runtime_error(llvm::toString(Parsed.takeError()));
  return std::move(*Parsed);
}
std::string field(const Value &V, const char *Name) {
  const auto *O = V.getAsObject();
  if (!O || !O->getString(Name))
    throw std::runtime_error(std::string("Missing field ") + Name);
  return O->getString(Name)->str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()
      ->getObject("error")
      ->getString("code")
      .value_or("")
      .str();
}
const llvm::json::Array &items(const Value &V) {
  return *V.getAsObject()->getArray("items");
}
bool hasASAR() {
  const auto V = take(neverd_web_capabilities_json());
  for (const auto &Op : *V.getAsObject()->getArray("operations"))
    if (Op.getAsString() == "asar_extract")
      return true;
  return false;
}
class WebElectronSDK : public ::testing::Test {
protected:
  neverd_web_session_t Web = nullptr;
  fs::path Root, Input;
  std::string Revision;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "Windows web capture is not implemented";
#endif
    Web = neverd_web_session_create();
    ASSERT_NE(Web, nullptr);
    llvm::SmallString<128> P;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-electron", P));
    Root = P.str().str();
    Input = Root / "input";
    fs::create_directory(Input);
  }
  void TearDown() override {
    neverd_web_session_destroy(Web);
    if (!Root.empty()) {
      std::error_code EC;
      fs::remove_all(Root, EC);
    }
  }
  void write(std::string_view Name, std::string_view Bytes) {
    const auto P = Input / std::string(Name);
    fs::create_directories(P.parent_path());
    std::ofstream F(P, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(F.good());
  }
  void capture(fs::path P = {}) {
    const auto Path = (P.empty() ? Input : P).string();
    const auto Preview = take(neverd_web_import_preview_json(
        Web, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    const auto Commit =
        take(neverd_web_import_commit_json(Web, Token.data(), Token.size()));
    Revision = field(Commit, "revision");
  }
  Value artifacts() {
    return take(neverd_web_artifacts_json(Web, Revision.data(), Revision.size(),
                                          0, 512));
  }
  std::string artifact(std::string_view Bytes) {
    const auto Page = artifacts();
    for (const auto &Item : items(Page))
      if (Item.getAsObject()->getString("blob_sha256") == asarHash(Bytes))
        return field(Item, "artifact_id");
    throw std::runtime_error("Fixture artifact absent");
  }
  Value manifest(std::string_view ID) {
    return take(neverd_web_electron_manifest_analyze_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  std::string source(std::string_view ID) {
    return field(take(neverd_web_source_analyze_json(Web, Revision.data(),
                                                     Revision.size(), ID.data(),
                                                     ID.size(), "commonjs", 8)),
                 "source_id");
  }
  Value analyze(std::string_view ID) {
    return take(neverd_web_electron_source_analyze_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value records(std::string_view ID, uint64_t Offset = 0,
                uint64_t Limit = 512) {
    return take(neverd_web_electron_source_records_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
  Value ipc(std::string_view Manifest, const std::vector<std::string> &IDs) {
    llvm::json::Array Selected;
    for (const auto &ID : IDs)
      Selected.emplace_back(ID);
    const auto Options = asarJSON(llvm::json::Object{
        {"schema_version", 1}, {"source_ids", std::move(Selected)}});
    return take(neverd_web_electron_ipc_analyze_json(
        Web, Revision.data(), Revision.size(), Manifest.data(), Manifest.size(),
        Options.data(), Options.size()));
  }
  Value ipcRecords(std::string_view ID, std::string_view Kind,
                   uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_electron_ipc_records_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  Value entries(std::string_view Manifest,
                const std::vector<std::string> &IDs) {
    llvm::json::Array Selected;
    for (const auto &ID : IDs)
      Selected.emplace_back(ID);
    const auto Options = asarJSON(llvm::json::Object{
        {"schema_version", 1}, {"source_ids", std::move(Selected)}});
    return take(neverd_web_electron_entries_analyze_json(
        Web, Revision.data(), Revision.size(), Manifest.data(), Manifest.size(),
        Options.data(), Options.size()));
  }
  Value entryRecords(std::string_view ID, std::string_view Kind,
                     uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_electron_entry_records_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  void cli(std::vector<std::string> Arguments, int Expected,
           std::string_view Contains) {
#ifdef NEVERD_WEB_TEST_CLI
    SCOPED_TRACE(Arguments.front());
    std::vector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web"};
    for (const auto &A : Arguments)
      Args.emplace_back(A);
    const auto Out = (Root / "out").string(), Err = (Root / "err").string();
    const std::optional<llvm::StringRef> Redirects[]{llvm::StringRef(), Out,
                                                     Err};
    const llvm::StringRef Env[]{"PATH=/neverd-no-external-tools",
                                "NEVERD_SIGNATURE_CACHE=off"};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              Expected);
    std::string Bytes;
    for (const auto &P : {Out, Err}) {
      std::ifstream F(P);
      Bytes.append(std::istreambuf_iterator<char>(F), {});
    }
    EXPECT_NE(Bytes.find(Contains), std::string::npos);
    EXPECT_EQ(Bytes.find("CANARY"), std::string::npos);
#else
    GTEST_SKIP() << "CLI not built";
#endif
  }
};

TEST_F(WebElectronSDK,
       DirectoryManifestSourceEvidenceAndAnchorsShareCapturedIdentity) {
  const std::string JSON =
      R"({"main":"CANARY-main.js","name":"CANARY-app","type":"commonjs"})";
  const std::string JS =
      R"JS(const {BrowserWindow,ipcMain} = require('electron');
    const win = new BrowserWindow({webPreferences:{preload:'CANARY-preload.js'}});
    ipcMain.handle('CANARY-channel', () => {throw Error('must not run');});)JS";
  write("package.json", JSON);
  write("CANARY-main.js", JS);
  capture();
  const auto ManifestID = artifact(JSON), MainID = artifact(JS);
  const auto M = manifest(ManifestID);
  EXPECT_EQ(field(M, "main_artifact_id"), MainID);
  EXPECT_EQ(field(M, "main_link_status"), "exact_admitted_file_candidate");
  EXPECT_EQ(M.getAsObject()->getBoolean("framework_verified"), false);
  EXPECT_EQ(field(take(neverd_web_metadata_json(Web)), "analysis_status"),
            "partial");
  EXPECT_EQ(field(artifacts(), "analysis_status"), "partial");
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto S = source(MainID);
  EXPECT_EQ(code(records(S)), "electron_source_not_analyzed");
  const auto A = analyze(S);
  EXPECT_EQ(A.getAsObject()->getInteger("boundary_count"), 3);
  EXPECT_EQ(field(A, "analysis_status"), "partial");
  const auto Page = records(S);
  ASSERT_EQ(items(Page).size(), 3U);
  const auto &IPC = items(Page).back();
  EXPECT_EQ(field(IPC, "kind"), "ipc_main_handle");
  const auto Offset = std::stoull(field(IPC, "byte_offset"));
  const auto Size = std::stoull(field(IPC, "byte_length"));
  const auto Anchor = take(neverd_web_source_anchor_json(
      Web, Revision.data(), Revision.size(), S.data(), S.size(), Offset, Size,
      nullptr, 0));
  EXPECT_EQ(
      Anchor.getAsObject()->getObject("storage")->getString("artifact_id"),
      MainID);
  EXPECT_EQ(code(records(S, 0, 0)), "invalid_page");
  EXPECT_EQ(code(records(S, UINT64_MAX, 1)), "invalid_page");
  EXPECT_EQ(items(records(S, 3, 1)).size(), 0U);
  fs::remove_all(Input);
  EXPECT_EQ(field(analyze(S), "electron_source_id"),
            field(A, "electron_source_id"));
  EXPECT_EQ(field(manifest(ManifestID), "manifest_id"),
            field(M, "manifest_id"));
#else
  EXPECT_EQ(code(analyze(MainID)), "capability_unavailable");
  EXPECT_EQ(code(records(MainID)), "capability_unavailable");
#endif
}

TEST_F(WebElectronSDK,
       InvalidManifestBudgetCacheAndRevisionChangesFailClearly) {
  const std::string Bad = R"({"main":"CANARY-a","main":"CANARY-b"})";
  write("bad.json", Bad);
  write("huge.json", std::string(1024 * 1024 + 1, ' '));
  for (unsigned I = 0; I < 17; ++I)
    write(std::to_string(I) + ".json",
          "{\"name\":\"CANARY-" + std::to_string(I) + "\"}");
  capture();
  EXPECT_EQ(field(manifest(artifact(Bad)), "status"), "error");
  EXPECT_EQ(code(manifest(artifact(std::string(1024 * 1024 + 1, ' ')))),
            "electron_manifest_budget_exceeded");
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_manifest_count"),
            0);
  std::string First;
  for (unsigned I = 0; I < 17; ++I) {
    const auto ID = artifact("{\"name\":\"CANARY-" + std::to_string(I) + "\"}");
    const auto M = manifest(ID);
    if (I < 16)
      EXPECT_EQ(field(M, "status"), "ok");
    else
      EXPECT_EQ(code(M), "electron_manifest_cache_budget_exceeded");
    if (!I) {
      First = ID;
      EXPECT_EQ(field(manifest(ID), "manifest_id"), field(M, "manifest_id"));
    }
  }
  const auto Previous = Revision;
  write("changed", "new");
  capture();
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_manifest_count"),
            0);
  EXPECT_EQ(code(manifest(First)), "unknown_artifact");
  EXPECT_EQ(
      code(take(neverd_web_electron_manifest_analyze_json(
          Web, Previous.data(), Previous.size(), First.data(), First.size()))),
      "stale_revision");
}

TEST_F(WebElectronSDK, ASARPackedManifestLinksAnExplicitCapturedUnpackedMain) {
  if (!hasASAR())
    GTEST_SKIP() << "Requires the fixed ASAR path policy";
  const std::string JSON = R"({"main":"CANARY-main.js"})";
  const std::string JS = "const {ipcRenderer} = require('electron'); "
                         "ipcRenderer.send('CANARY-channel');";
  const auto Archive = asarArchive({{"package.json", asarFile(JSON)},
                                    {"CANARY-main.js", asarFile(JS, 0, true)}},
                                   JSON);
  write("a.asar", Archive);
  write("chosen/CANARY-main.js", JS);
  capture();
  std::string Directory;
  const auto Original = artifacts();
  for (size_t I = 1; I < items(Original).size(); ++I)
    if (items(Original)[I].getAsObject()->getString("kind") == "directory")
      Directory = field(items(Original)[I], "artifact_id");
  const auto ID = artifact(Archive);
  const auto Extracted = take(neverd_web_asar_extract_json(
      Web, Revision.data(), Revision.size(), ID.data(), ID.size(),
      Directory.data(), Directory.size()));
  const auto E = field(Extracted, "extraction_id");
  const auto Page = take(neverd_web_asar_records_json(
      Web, Revision.data(), Revision.size(), E.data(), E.size(), 0, 512));
  std::string ManifestID, MainID;
  for (const auto &M : items(Page)) {
    if (field(M, "blob_sha256") == asarHash(JSON))
      ManifestID = field(M, "member_id");
    if (field(M, "blob_sha256") == asarHash(JS))
      MainID = field(M, "member_id");
  }
  const auto M = manifest(ManifestID);
  EXPECT_EQ(field(M, "main_artifact_id"), MainID);
  EXPECT_EQ(M.getAsObject()->getObject("origin")->getString("kind"),
            "asar_packed_member");
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto S = source(MainID);
  EXPECT_EQ(analyze(S).getAsObject()->getInteger("boundary_count"), 1);
  const auto Correlation = ipc(ManifestID, {S});
  EXPECT_EQ(field(Correlation, "namespace_id"), E);
  const auto IPCID = field(Correlation, "electron_ipc_id");
  const auto Endpoints = ipcRecords(IPCID, "endpoints");
  ASSERT_EQ(items(Endpoints).size(), 1U);
  EXPECT_EQ(field(items(Endpoints)[0], "artifact_id"), MainID);
  // Equal backing bytes outside this ASAR occurrence cannot supply membership.
  const auto External = source(artifact(JS));
  analyze(External);
  EXPECT_EQ(code(ipc(ManifestID, {S, External})),
            "electron_ipc_source_outside_namespace");
  const auto Anchor = take(neverd_web_source_anchor_json(
      Web, Revision.data(), Revision.size(), S.data(), S.size(), 0, JS.size(),
      nullptr, 0));
  EXPECT_EQ(Anchor.getAsObject()->getObject("storage")->getString("kind"),
            "asar_unpacked_member");
  EXPECT_EQ(Anchor.getAsObject()->getObject("storage")->getString(
                "storage_artifact_id"),
            artifact(JS));
  const auto Previous = Revision;
  write("changed", "new snapshot");
  capture();
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_source_count"),
            0);
  EXPECT_EQ(code(records(S)), "unknown_source");
  EXPECT_EQ(code(ipcRecords(IPCID, "channels")),
            "unknown_electron_ipc_analysis");
  EXPECT_EQ(
      code(take(neverd_web_electron_source_records_json(
          Web, Previous.data(), Previous.size(), S.data(), S.size(), 0, 1))),
      "stale_revision");
#endif
}

TEST_F(WebElectronSDK, CLIMetadataAndSourceBoundariesWorkWithoutExternalTools) {
  const std::string JSON = R"({"main":"a.js","name":"CANARY-app"})";
  const std::string JS = "const {ipcMain} = require('electron'); "
                         "ipcMain.handle('CANARY-channel', fn);";
  write("a.js", JS);
  write("package.json", JSON);
  cli({"electron-manifest", Input.string(), "2"}, 0,
      "exact_admitted_file_candidate");
  cli({"electron-manifest", (Input / "package.json").string()}, 0,
      "no_directory_namespace");
  cli({"electron-manifest", Input.string(), "02"}, 2,
      "invalid_manifest_selection");
  cli({"electron-ipc", Input.string(), "2", "01:commonjs"}, 2,
      "invalid_ipc_source_selection");
  cli({"electron-entries", Input.string(), "2", "01:commonjs"}, 2,
      "invalid_ipc_source_selection");
#if NEVERD_TEST_WEB_JAVASCRIPT
  cli({"electron-source", (Input / "a.js").string(), "commonjs"}, 0,
      "ipc_main_handle");
  cli({"electron-ipc", Input.string(), "2", "1:commonjs"}, 0,
      "invoke_handle_candidate_pairs");
  cli({"electron-entries", Input.string(), "2", "1:commonjs"}, 0,
      "electron_entries_id");
#else
  cli({"electron-source", (Input / "a.js").string(), "commonjs"}, 1,
      "capability_unavailable");
  cli({"electron-ipc", Input.string(), "2", "1:commonjs"}, 1,
      "capability_unavailable");
  cli({"electron-entries", Input.string(), "2", "1:commonjs"}, 1,
      "capability_unavailable");
#endif
  if (hasASAR()) {
    const auto Archive = asarArchive(
        {{"a.js", asarFile(JS)}, {"package.json", asarFile(JSON, JS.size())}},
        JS + JSON);
    write("a.asar", Archive);
    cli({"asar-manifest", (Input / "a.asar").string(), "0", "-", "1"}, 0,
        "exact_admitted_file_candidate");
#if NEVERD_TEST_WEB_JAVASCRIPT
    cli({"asar-electron", (Input / "a.asar").string(), "0", "-", "0",
         "commonjs"},
        0, "ipc_main_handle");
    cli({"asar-ipc", (Input / "a.asar").string(), "0", "-", "1", "0:commonjs"},
        0, "invoke_handle_candidate_pairs");
    cli({"asar-entries", (Input / "a.asar").string(), "0", "-", "1",
         "0:commonjs"},
        0, "electron_entries_id");
#else
    cli({"asar-entries", (Input / "a.asar").string(), "0", "-", "1",
         "0:commonjs"},
        1, "capability_unavailable");
#endif
  }
}
TEST_F(WebElectronSDK, IPCScopePagesCacheAndRevisionsKeepChannelValuesPrivate) {
  const std::string JSON = R"({"main":"main.js"})";
  const std::string Main = "const {ipcMain}=require('electron'); "
                           "ipcMain.handle('CANARY-channel', f);";
  const std::string Preload =
      "const {ipcRenderer}=require('electron'); "
      "ipcRenderer.invoke('CANARY-channel'); ipcRenderer.send(dynamic);";
  write("app/package.json", JSON);
  write("app/main.js", Main);
  write("app/preload.js", Preload);
  capture();
  const auto M = artifact(JSON);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto S = source(artifact(Main)), P = source(artifact(Preload));
  EXPECT_EQ(code(ipc(M, {S, P})), "electron_manifest_not_analyzed");
  manifest(M);
  EXPECT_EQ(code(ipc(M, {S, P})), "electron_source_not_analyzed");
  analyze(S);
  analyze(P);
  const auto A = ipc(M, {S, P});
  const auto ID = field(A, "electron_ipc_id");
  EXPECT_EQ(A.getAsObject()->getInteger("channel_count"), 1);
  EXPECT_EQ(A.getAsObject()->getInteger("endpoint_count"), 3);
  EXPECT_EQ(A.getAsObject()->getInteger("unresolved_endpoint_count"), 1);
  EXPECT_EQ(A.getAsObject()->getBoolean("runtime_routing_verified"), false);
  const auto Channels = ipcRecords(ID, "channels");
  ASSERT_EQ(items(Channels).size(), 1U);
  EXPECT_EQ(items(Channels)[0].getAsObject()->getInteger(
                "invoke_handle_candidate_pairs"),
            1);
  const auto ChannelID = field(items(Channels)[0], "channel_id");
  const auto Sources = ipcRecords(ID, "sources");
  EXPECT_EQ(items(Sources).size(), 2U);
  size_t Matched = 0;
  for (uint64_t I = 0; I < 3; ++I) {
    const auto Page = ipcRecords(ID, "endpoints", I, 1);
    ASSERT_EQ(items(Page).size(), 1U);
    const auto &Endpoint = items(Page)[0];
    if (Endpoint.getAsObject()->getString("channel_id") == ChannelID)
      ++Matched;
    const auto SourceID = field(Endpoint, "source_id");
    const auto Offset = std::stoull(field(Endpoint, "byte_offset"));
    const auto Length = std::stoull(field(Endpoint, "byte_length"));
    const auto Anchor = take(neverd_web_source_anchor_json(
        Web, Revision.data(), Revision.size(), SourceID.data(), SourceID.size(),
        Offset, Length, nullptr, 0));
    EXPECT_EQ(
        Anchor.getAsObject()->getObject("storage")->getString("artifact_id"),
        field(Endpoint, "artifact_id"));
  }
  EXPECT_EQ(Matched, 2U);
  EXPECT_EQ(field(ipc(M, {P, S}), "electron_ipc_id"), ID);
  EXPECT_EQ(code(ipc(M, {P})), "electron_ipc_main_source_required");
  EXPECT_EQ(code(ipc(M, {S, S})), "electron_ipc_duplicate_source_artifact");
  EXPECT_EQ(code(ipcRecords(ID, "channels", 0, 0)), "invalid_page");
  EXPECT_EQ(code(ipcRecords(ID, "endpoints", UINT64_MAX, 1)), "invalid_page");
  EXPECT_EQ(code(ipcRecords(ID, "CANARY-unknown")), "invalid_record_kind");
  fs::remove_all(Input);
  EXPECT_EQ(field(ipc(M, {S, P}), "electron_ipc_id"), ID);
  EXPECT_EQ(items(ipcRecords(ID, "endpoints")).size(), 3U);
  const auto Old = Revision;
  write("new", "new capture");
  capture();
  EXPECT_EQ(code(ipcRecords(ID, "endpoints")), "unknown_electron_ipc_analysis");
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_ipc_count"),
            0);
  EXPECT_EQ(code(take(neverd_web_electron_ipc_records_json(
                Web, Old.data(), Old.size(), ID.data(), ID.size(), "channels",
                8, 0, 1))),
            "stale_revision");
#else
  EXPECT_EQ(code(ipc(M, {"source"})), "capability_unavailable");
  EXPECT_EQ(code(ipcRecords("analysis", "channels")), "capability_unavailable");
#endif
}

TEST_F(WebElectronSDK, EntryPagesScopesAnchorsAndRevisionsKeepPathsPrivate) {
  const std::string JSON = R"({"main":"bin/main.js"})";
  const std::string Main = R"JS(
    const {BrowserWindow}=require('electron'); const path=require('path');
    const win=new BrowserWindow({webPreferences:{preload:path.join(__dirname,'..','CANARY-preload.js')}});
    win.loadFile('CANARY-index.html'); win.loadURL('https://example.invalid/CANARY');
    win.loadFile(unknownPath);
  )JS";
  const std::string Preload = "const privateValue = 'CANARY-source';";
  const std::string HTML = "<script src='CANARY-renderer.js'></script>";
  write("app/package.json", JSON);
  write("app/bin/main.js", Main);
  write("app/CANARY-preload.js", Preload);
  write("app/CANARY-index.html", HTML);
  capture();
  const auto M = artifact(JSON);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto MainArtifact = artifact(Main), PreloadArtifact = artifact(Preload),
             HTMLArtifact = artifact(HTML);
  const auto S = source(MainArtifact), P = source(PreloadArtifact);
  EXPECT_EQ(code(entries(M, {S, P})), "electron_manifest_not_analyzed");
  manifest(M);
  EXPECT_EQ(code(entries(M, {S, P})), "electron_source_not_analyzed");
  analyze(S);
  analyze(P);
  const auto A = entries(M, {S, P});
  const auto ID = field(A, "electron_entries_id");
  EXPECT_EQ(A.getAsObject()->getInteger("entry_count"), 4);
  EXPECT_EQ(A.getAsObject()->getInteger("linked_file_candidate_count"), 2);
  EXPECT_EQ(A.getAsObject()->getBoolean("runtime_entries_verified"), false);
  EXPECT_EQ(A.getAsObject()->getBoolean("runtime_path_bases_verified"), false);
  EXPECT_EQ(field(A, "html_analysis"), "not_analyzed");
  EXPECT_EQ(items(entryRecords(ID, "sources")).size(), 2U);
  const auto Page = entryRecords(ID, "entries");
  ASSERT_EQ(items(Page).size(), 4U);
  EXPECT_EQ(field(items(Page)[0], "target_artifact_id"), PreloadArtifact);
  EXPECT_EQ(field(items(Page)[0], "selected_target_source_id"), P);
  EXPECT_EQ(field(items(Page)[1], "target_artifact_id"), HTMLArtifact);
  EXPECT_TRUE(items(Page)[1]
                  .getAsObject()
                  ->get("selected_target_source_id")
                  ->getAsNull());
  EXPECT_EQ(field(items(Page)[2], "link_status"), "url_base_not_captured");
  EXPECT_EQ(field(items(Page)[3], "link_status"),
            "unsupported_path_expression");
  for (uint64_t I = 0; I < 4; ++I) {
    const auto One = entryRecords(ID, "entries", I, 1);
    ASSERT_EQ(items(One).size(), 1U);
    const auto &E = items(One)[0];
    EXPECT_EQ(field(E, "entry_id"), field(items(Page)[I], "entry_id"));
    EXPECT_EQ(E.getAsObject()->getBoolean("path_redacted"), true);
    const auto Offset = std::stoull(field(E, "byte_offset"));
    const auto Length = std::stoull(field(E, "byte_length"));
    const auto Anchor = take(neverd_web_source_anchor_json(
        Web, Revision.data(), Revision.size(), S.data(), S.size(), Offset,
        Length, nullptr, 0));
    EXPECT_EQ(
        Anchor.getAsObject()->getObject("storage")->getString("artifact_id"),
        MainArtifact);
  }
  EXPECT_EQ(field(entries(M, {P, S}), "electron_entries_id"), ID);
  EXPECT_EQ(code(entries(M, {P})), "electron_ipc_main_source_required");
  EXPECT_EQ(code(entries(M, {S, S})), "electron_ipc_duplicate_source_artifact");
  EXPECT_EQ(code(entryRecords(ID, "entries", 0, 0)), "invalid_page");
  EXPECT_EQ(code(entryRecords(ID, "entries", 0, 513)), "invalid_page");
  EXPECT_EQ(code(entryRecords(ID, "entries", UINT64_MAX, 1)), "invalid_page");
  EXPECT_EQ(code(entryRecords(ID, "CANARY")), "invalid_record_kind");
  const auto Metadata = take(neverd_web_metadata_json(Web));
  EXPECT_EQ(Metadata.getAsObject()->getInteger("electron_entries_count"), 1);
  EXPECT_EQ(Metadata.getAsObject()->getInteger("electron_ipc_count"), 0);
  fs::remove_all(Input);
  EXPECT_EQ(field(entries(M, {S, P}), "electron_entries_id"), ID);
  EXPECT_EQ(items(entryRecords(ID, "entries")).size(), 4U);
  const auto Old = Revision;
  write("new", "replacement snapshot");
  capture();
  EXPECT_EQ(code(entryRecords(ID, "entries")),
            "unknown_electron_entries_analysis");
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_entries_count"),
            0);
  EXPECT_EQ(code(take(neverd_web_electron_entry_records_json(
                Web, Old.data(), Old.size(), ID.data(), ID.size(), "entries", 7,
                0, 1))),
            "stale_revision");
#else
  EXPECT_EQ(code(entries(M, {"source"})), "capability_unavailable");
  EXPECT_EQ(code(entryRecords("analysis", "entries")),
            "capability_unavailable");
#endif
}

TEST_F(WebElectronSDK, ASAREntriesAndCLIUseCapturedMemberOccurrences) {
  if (!hasASAR())
    GTEST_SKIP() << "Pinned ICU ASAR capability unavailable";
  const std::string JSON = R"({"main":"a.js"})";
  const std::string Main = R"JS(const {BrowserWindow}=require('electron');
    const path=require('path');
    const win=new BrowserWindow({webPreferences:{preload:path.join(__dirname,'CANARY-preload.js')}});
    win.loadFile('CANARY-index.html'); win.loadFile('CANARY-absent.html');)JS";
  const std::string Preload = "const saved='CANARY';";
  const std::string HTML = "<p>CANARY</p>";
  const auto Archive = asarArchive(
      {{"a.js", asarFile(Main)},
       {"CANARY-preload.js", asarFile(Preload, Main.size())},
       {"CANARY-index.html", asarFile(HTML, Main.size() + Preload.size())},
       {"CANARY-absent.html", asarFile("not captured", 0, true)},
       {"package.json",
        asarFile(JSON, Main.size() + Preload.size() + HTML.size())}},
      Main + Preload + HTML + JSON);
  write("app.asar", Archive);
  write("outside.js", Preload);
  capture();
  const auto ArchiveID = artifact(Archive);
  const auto Extraction = take(neverd_web_asar_extract_json(
      Web, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
      nullptr, 0));
  const auto ExtractionID = field(Extraction, "extraction_id");
  const auto Members = take(neverd_web_asar_records_json(
      Web, Revision.data(), Revision.size(), ExtractionID.data(),
      ExtractionID.size(), 0, 128));
  std::string ManifestID, MainID, PreloadID, HTMLID;
  uint64_t ManifestIndex = 0, MainIndex = 0, PreloadIndex = 0;
  for (uint64_t I = 0; I < items(Members).size(); ++I) {
    const auto &Item = items(Members)[I];
    const auto Hash = Item.getAsObject()->getString("blob_sha256");
    if (Hash == asarHash(JSON)) {
      ManifestID = field(Item, "member_id");
      ManifestIndex = I;
    }
    if (Hash == asarHash(Main)) {
      MainID = field(Item, "member_id");
      MainIndex = I;
    }
    if (Hash == asarHash(Preload)) {
      PreloadID = field(Item, "member_id");
      PreloadIndex = I;
    }
    if (Hash == asarHash(HTML))
      HTMLID = field(Item, "member_id");
  }
  manifest(ManifestID);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto S = source(MainID), P = source(PreloadID);
  analyze(S);
  analyze(P);
  const auto A = entries(ManifestID, {S, P});
  EXPECT_EQ(field(A, "namespace_id"), ExtractionID);
  EXPECT_EQ(A.getAsObject()->getInteger("linked_file_candidate_count"), 2);
  const auto Page = entryRecords(field(A, "electron_entries_id"), "entries");
  ASSERT_EQ(items(Page).size(), 3U);
  EXPECT_EQ(field(items(Page)[0], "target_artifact_id"), PreloadID);
  EXPECT_EQ(field(items(Page)[1], "target_artifact_id"), HTMLID);
  EXPECT_EQ(field(items(Page)[2], "link_status"), "no_exact_admitted_file");
  const auto External = source(artifact(Preload));
  analyze(External);
  EXPECT_EQ(code(entries(ManifestID, {S, External})),
            "electron_ipc_source_outside_namespace");
  cli({"asar-entries", (Input / "app.asar").string(), "0", "-",
       std::to_string(ManifestIndex), std::to_string(MainIndex) + ":commonjs",
       std::to_string(PreloadIndex) + ":commonjs"},
      0, "selected_target_source_id");
#else
  cli({"asar-entries", (Input / "app.asar").string(), "0", "-",
       std::to_string(ManifestIndex), std::to_string(MainIndex) + ":commonjs"},
      1, "capability_unavailable");
#endif
}

#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebElectronSDK, EntryInvalidOptionsAndCacheLimitDoNotPublishResults) {
  const std::string JSON = R"({"main":"main.js"})";
  const std::string Main =
      "const {BrowserWindow}=require('electron'); const win=new "
      "BrowserWindow(); win.loadFile('CANARY.html');";
  write("app/package.json", JSON);
  write("app/main.js", Main);
  for (unsigned I = 0; I < 5; ++I)
    write("app/part" + std::to_string(I) + ".js", "void " + std::to_string(I));
  capture();
  const auto M = artifact(JSON), S = source(artifact(Main));
  manifest(M);
  analyze(S);
  for (const auto Options :
       {R"({})", R"({"schema_version":1,"source_ids":[]})",
        R"({"schema_version":1,"source_ids":[3]})",
        R"({"schema_version":1,"source_ids":["CANARY"],"execute":true})"})
    EXPECT_EQ(field(take(neverd_web_electron_entries_analyze_json(
                        Web, Revision.data(), Revision.size(), M.data(),
                        M.size(), Options, std::strlen(Options))),
                    "status"),
              "error");
  std::string First, FirstSource;
  for (unsigned I = 0; I < 5; ++I) {
    const auto P = source(artifact("void " + std::to_string(I)));
    analyze(P);
    const auto A = entries(M, {S, P});
    if (I < 4) {
      EXPECT_EQ(field(A, "status"), "ok");
      if (!I) {
        First = field(A, "electron_entries_id");
        FirstSource = P;
      }
    } else
      EXPECT_EQ(code(A), "electron_entries_cache_budget_exceeded");
  }
  EXPECT_EQ(field(entries(M, {S, FirstSource}), "electron_entries_id"), First);
  EXPECT_EQ(items(entryRecords(First, "entries")).size(), 1U);
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_entries_count"),
            4);
}

TEST_F(WebElectronSDK,
       IPCInvalidOptionsScopeAndCacheLimitsDoNotPublishResults) {
  const std::string JSON = R"({"main":"main.js"})";
  const std::string Main =
      "const {ipcMain}=require('electron'); ipcMain.handle('CANARY', f);";
  write("app/package.json", JSON);
  write("app/main.js", Main);
  for (unsigned I = 0; I < 5; ++I)
    write("app/part" + std::to_string(I) + ".js", "void " + std::to_string(I));
  write("application/other.js", "void 123");
  capture();
  const auto M = artifact(JSON), S = source(artifact(Main));
  manifest(M);
  analyze(S);
  for (const auto Options :
       {R"({})", R"({"schema_version":1,"source_ids":[]})",
        R"({"schema_version":1,"source_ids":[3]})",
        R"({"schema_version":1,"source_ids":["CANARY"],"execute":true})",
        R"({"schema_version":1,"source_ids":[],"source_ids":[]})"}) {
    EXPECT_EQ(field(take(neverd_web_electron_ipc_analyze_json(
                        Web, Revision.data(), Revision.size(), M.data(),
                        M.size(), Options, std::strlen(Options))),
                    "status"),
              "error");
  }
  const auto Outside = source(artifact("void 123"));
  analyze(Outside);
  EXPECT_EQ(code(ipc(M, {S, Outside})),
            "electron_ipc_source_outside_manifest_scope");
  std::string First, FirstSource;
  for (unsigned I = 0; I < 5; ++I) {
    const auto P = source(artifact("void " + std::to_string(I)));
    analyze(P);
    const auto A = ipc(M, {S, P});
    if (I < 4) {
      EXPECT_EQ(field(A, "status"), "ok");
      if (!I) {
        First = field(A, "electron_ipc_id");
        FirstSource = P;
      }
    } else
      EXPECT_EQ(code(A), "electron_ipc_cache_budget_exceeded");
  }
  EXPECT_EQ(field(ipc(M, {S, FirstSource}), "electron_ipc_id"), First);
  EXPECT_EQ(items(ipcRecords(First, "channels")).size(), 1U);
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("electron_ipc_count"),
            4);
}
#endif
} // namespace
