//===- HTMLSDKTests.cpp - Captured HTML script evidence tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured HTML script evidence tests.
///
//===----------------------------------------------------------------------===//

#include "AsarFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using namespace neverd::web::test;
using llvm::json::Value;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("Null HTML API response");
  const std::string Bytes(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Bytes.find("CANARY"), Bytes.npos);
  auto V = llvm::json::parse(Bytes);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
  return std::move(*V);
}
std::string field(const Value &V, const char *Key) {
  const auto *O = V.getAsObject();
  if (!O || !O->getString(Key))
    throw std::runtime_error(std::string("Missing ") + Key);
  return O->getString(Key)->str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()->getObject("error")->getString("code")->str();
}
const llvm::json::Array &items(const Value &V) {
  return *V.getAsObject()->getArray("items");
}
bool has(std::string_view Operation) {
  const auto V = take(neverd_web_capabilities_json());
  for (const auto &O : *V.getAsObject()->getArray("operations"))
    if (O.getAsString() == llvm::StringRef(Operation))
      return true;
  return false;
}
class WebHTMLSDK : public ::testing::Test {
protected:
  neverd_web_session_t Web = nullptr;
  fs::path Root, Input;
  std::string Revision;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "Windows web capture is unavailable";
#endif
    Web = neverd_web_session_create();
    ASSERT_NE(Web, nullptr);
    llvm::SmallString<128> P;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-html", P));
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
  void write(std::string_view Name, std::string_view Text) {
    const auto P = Input / std::string(Name);
    fs::create_directories(P.parent_path());
    std::ofstream F(P, std::ios::binary);
    F.write(Text.data(), Text.size());
    ASSERT_TRUE(F.good());
  }
  void capture() {
    const auto P = Input.string();
    const auto Preview = take(
        neverd_web_import_preview_json(Web, P.data(), P.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(Web, Token.data(), Token.size())),
        "revision");
  }
  std::string artifact(std::string_view Bytes) {
    const auto Page = take(neverd_web_artifacts_json(Web, Revision.data(),
                                                     Revision.size(), 0, 512));
    for (const auto &Item : items(Page))
      if (Item.getAsObject()->getString("blob_sha256") == asarHash(Bytes))
        return field(Item, "artifact_id");
    throw std::runtime_error("Missing HTML fixture artifact");
  }
  Value analyze(std::string_view ID) {
    return take(neverd_web_html_analyze_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value records(std::string_view ID, std::string_view Kind = "scripts",
                uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_html_records_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  Value source(std::string_view ID, std::string_view Type) {
    return take(neverd_web_source_analyze_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Type.data(), Type.size()));
  }
  Value moduleRequests(std::string_view ID) {
    const auto Summary = take(neverd_web_source_modules_analyze_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size()));
    EXPECT_EQ(field(Summary, "link_profile"),
              "html-inline-module-file-candidates-v2");
    return take(neverd_web_source_module_records_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(), "requests",
        8, 0, 512));
  }
  Value anchor(std::string_view ID, uint64_t At, uint64_t Length) {
    return take(neverd_web_source_anchor_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(), At, Length,
        nullptr, 0));
  }
  void cli(std::vector<std::string> Arguments, int Exit,
           std::string_view Contains) {
#ifdef NEVERD_WEB_TEST_CLI
    std::vector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web"};
    for (const auto &A : Arguments)
      Args.emplace_back(A);
    const auto Out = (Root / "out").string(), Err = (Root / "err").string();
    const std::optional<llvm::StringRef> Redirect[]{llvm::StringRef(), Out,
                                                    Err};
    const llvm::StringRef Env[]{"PATH=/no-external-tools",
                                "NEVERD_SIGNATURE_CACHE=off"};
    const auto ActualExit =
        llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env, Redirect, 30);
    std::string Text;
    for (const auto &P : {Out, Err}) {
      std::ifstream F(P);
      Text.append(std::istreambuf_iterator<char>(F), {});
    }
    EXPECT_EQ(ActualExit, Exit) << Text;
    EXPECT_NE(Text.find(Contains), Text.npos) << Text;
    EXPECT_EQ(Text.find("CANARY"), Text.npos);
#else
    GTEST_SKIP() << "CLI not built";
#endif
  }
};

TEST_F(WebHTMLSDK, InventoryLinksArePrivateAndSurviveCapturedInputDeletion) {
  const std::string HTML =
      "<base href='./CANARY-dir/'><script "
      "src='CANARY-app.js?CANARY-query#CANARY-fragment'></script><script "
      "type=module>const secret='CANARY-value';import "
      "'./CANARY-module.js';</script><script "
      "src=missing></script>";
  const std::string JS = "throw Error('CANARY-not-run');";
  const std::string Dependency = "export const value='CANARY-module';";
  write("index.html", HTML);
  write("CANARY-dir/CANARY-app.js", JS);
  write("CANARY-dir/CANARY-module.js", Dependency);
  capture();
  ASSERT_TRUE(has("html_analyze"));
  ASSERT_TRUE(has("html_records"));
  const auto Doc = artifact(HTML), Target = artifact(JS),
             ModuleTarget = artifact(Dependency);
  const auto Analysis = analyze(Doc);
  const auto ID = field(Analysis, "html_id");
  EXPECT_EQ(field(Analysis, "analysis_status"), "partial");
  EXPECT_EQ(Analysis.getAsObject()->getBoolean("runtime_entries_verified"),
            false);
  const auto Page = records(ID);
  ASSERT_EQ(items(Page).size(), 3u);
  EXPECT_EQ(field(items(Page)[0], "candidate_artifact_id"), Target);
  EXPECT_EQ(items(Page)[0].getAsObject()->getBoolean("query_present"), true);
  EXPECT_EQ(items(Page)[0].getAsObject()->getBoolean("fragment_present"), true);
  EXPECT_EQ(field(items(Page)[1], "source_type"), "module");
  EXPECT_EQ(field(items(Page)[2], "link_status"), "not_in_snapshot");
  EXPECT_EQ(items(records(ID, "bases")).size(), 1u);
  const auto First = records(ID, "scripts", 0, 1);
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 1);
  EXPECT_EQ(code(records(ID, "scripts", 0, 0)), "invalid_page");
  EXPECT_EQ(code(records(ID, "scripts", 0, 513)), "invalid_page");
  EXPECT_EQ(code(records(ID, "scripts", 4)), "invalid_page");
  EXPECT_EQ(code(records(ID, "unknown")), "invalid_record_kind");
  EXPECT_EQ(code(records("unknown")), "unknown_html_analysis");
  fs::remove_all(Input);
  EXPECT_EQ(field(analyze(Doc), "html_id"), ID);
  const auto Inline = field(items(Page)[1], "inline_artifact_id");
  neverd_session_t Native = nullptr;
  EXPECT_EQ(code(take(neverd_web_native_open_json(
                Web, Revision.data(), Revision.size(), Inline.data(),
                Inline.size(), &Native))),
            "native_selection_unavailable");
  EXPECT_EQ(Native, nullptr);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Parsed = source(Inline, "module");
  EXPECT_EQ(field(Parsed, "parse_status"), "parsed");
  EXPECT_EQ(
      field(Parsed, "blob_sha256"),
      asarHash("const secret='CANARY-value';import './CANARY-module.js';"));
  const auto SourceID = field(Parsed, "source_id");
  const auto Modules = take(neverd_web_source_modules_analyze_json(
      Web, Revision.data(), Revision.size(), SourceID.data(), SourceID.size()));
  EXPECT_EQ(field(Modules, "status"), "ok");
  const auto Requests = take(neverd_web_source_module_records_json(
      Web, Revision.data(), Revision.size(), SourceID.data(), SourceID.size(),
      "requests", 8, 0, 512));
  ASSERT_EQ(items(Requests).size(), 1u);
  EXPECT_EQ(field(items(Requests)[0], "link_status"),
            "exact_admitted_file_candidate");
  EXPECT_EQ(field(items(Requests)[0], "candidate_artifact_id"), ModuleTarget);
  EXPECT_EQ(
      Modules.getAsObject()->getObject("link_context")->getString("html_id"),
      ID);
  EXPECT_EQ(field(Modules, "link_profile"),
            "html-inline-module-file-candidates-v2");
  const auto Anchor = anchor(field(Parsed, "source_id"), 6, 6);
  const auto *Storage = Anchor.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("byte_offset"),
            std::to_string(HTML.find("const secret") + 6));
  const auto *Origin = Storage;
  EXPECT_EQ(Origin->getString("kind"), "html_inline_script");
  EXPECT_EQ(Origin->getString("html_artifact_id"), Doc);
  EXPECT_EQ(Origin->getObject("parent_origin")->getString("kind"),
            "original_artifact");
#else
  EXPECT_EQ(code(source(Inline, "module")), "capability_unavailable");
#endif
  EXPECT_EQ(code(analyze(Inline)), "nested_html_analysis_unsupported");
}

TEST_F(WebHTMLSDK,
       CacheRevisionAndMalformedResultsNeverExposeStaleInlineSources) {
  std::vector<std::string> Documents;
  for (unsigned I = 0; I < 5; ++I) {
    Documents.push_back("<script>" + std::to_string(I) + "</script>");
    write(std::to_string(I) + ".html", Documents.back());
  }
  write("bad.html", "<script>CANARY</script><a x='");
  capture();
  const auto First = field(analyze(artifact(Documents[0])), "html_id");
  const auto Inline = field(items(records(First))[0], "inline_artifact_id");
  for (unsigned I = 1; I < 4; ++I)
    EXPECT_EQ(field(analyze(artifact(Documents[I])), "analysis_status"),
              "partial");
  EXPECT_EQ(code(analyze(artifact(Documents[4]))),
            "html_cache_budget_exceeded");
  const auto Old = Revision;
  capture();
  EXPECT_EQ(code(take(neverd_web_html_records_json(Web, Old.data(), Old.size(),
                                                   First.data(), First.size(),
                                                   "scripts", 7, 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(records(First)), "unknown_html_analysis");
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(code(source(Inline, "script")), "unknown_artifact");
#endif
  const auto Bad = analyze(artifact("<script>CANARY</script><a x='"));
  EXPECT_EQ(field(Bad, "analysis_status"), "unavailable");
  EXPECT_EQ(Bad.getAsObject()->getInteger("script_count"), 0);
  EXPECT_TRUE(items(records(field(Bad, "html_id"))).empty());
  EXPECT_EQ(code(take(neverd_web_html_analyze_json(
                Web, Revision.data(), Revision.size(), nullptr, 1))),
            "invalid_buffer");
}

TEST_F(WebHTMLSDK, InlineContextsStaySeparateAndDoNotRebaseExternalSources) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Requires JS parser";
#else
  const std::string External = "import './dep.js';";
  const std::string A = "<base href='./assets/'><script type=module "
                        "src='../shared/entry.js'></script><script "
                        "type=module>import './dep.js';</script>";
  const std::string B = "<base href='./other/'><script type=module "
                        "src='../shared/entry.js'></script><script "
                        "type=module>import './dep.js';</script>";
  write("a.html", A);
  write("b.html", B);
  write("shared/entry.js", External);
  write("assets/dep.js", "1;");
  write("other/dep.js", "2;");
  write("shared/dep.js", "3;");
  capture();
  const auto ExternalArtifact = artifact(External);
  const auto ExternalSource =
      field(source(ExternalArtifact, "module"), "source_id");
  const auto ExternalModules = [&] {
    return take(neverd_web_source_modules_analyze_json(
        Web, Revision.data(), Revision.size(), ExternalSource.data(),
        ExternalSource.size()));
  };
  const auto Before = ExternalModules();
  EXPECT_EQ(field(Before, "link_profile"), "admitted-relative-file-v1");
  std::vector<std::string> ContextIDs, InlineSources;
  for (const auto &[Text, Target] : {std::pair{A, "1;"}, std::pair{B, "2;"}}) {
    const auto H = analyze(artifact(Text));
    const auto Page = records(field(H, "html_id"));
    ASSERT_EQ(items(Page).size(), 2u);
    EXPECT_EQ(field(items(Page)[0], "candidate_artifact_id"), ExternalArtifact);
    const auto S =
        field(source(field(items(Page)[1], "inline_artifact_id"), "module"),
              "source_id");
    InlineSources.push_back(S);
    const auto Requests = moduleRequests(S);
    ASSERT_EQ(items(Requests).size(), 1u);
    EXPECT_EQ(field(items(Requests)[0], "candidate_artifact_id"),
              artifact(Target));
    const auto *Context = Requests.getAsObject()->getObject("link_context");
    ASSERT_NE(Context, nullptr);
    ContextIDs.push_back(Context->getString("context_id")->str());
    EXPECT_EQ(Context->getString("html_id"), field(H, "html_id"));
  }
  EXPECT_NE(ContextIDs[0], ContextIDs[1]);
  EXPECT_NE(InlineSources[0], InlineSources[1]);
  const auto After = ExternalModules();
  EXPECT_EQ(field(Before, "link_analysis_id"),
            field(After, "link_analysis_id"));
  EXPECT_TRUE(
      After.getAsObject()->get("link_context")->getAsNull().has_value());
  const auto Requests = take(neverd_web_source_module_records_json(
      Web, Revision.data(), Revision.size(), ExternalSource.data(),
      ExternalSource.size(), "requests", 8, 0, 512));
  ASSERT_EQ(items(Requests).size(), 1u);
  EXPECT_EQ(field(items(Requests)[0], "candidate_artifact_id"), artifact("3;"));
#endif
}

TEST_F(WebHTMLSDK, ImportMapInventoryIsBoundedPrivateAndUsesCapturedBytes) {
  const std::string HTML = R"(<script type=importmap>{"imports":{
      "CANARY-name":"./dep.js?CANARY-query#CANARY-fragment"},
      "integrity":{"./dep.js":"CANARY-digest"}}</script>
      <template><script type=importmap>{"CANARY":"private"}</script></template>
      <script type=module>import 'CANARY-name';</script>)";
  write("index.html", HTML);
  write("dep.js", "export const x=1;");
  capture();
  const auto Doc = artifact(HTML);
  fs::remove_all(Input);
  const auto Summary = analyze(Doc);
  EXPECT_EQ(field(Summary, "import_map_analysis"), "partial");
  EXPECT_EQ(Summary.getAsObject()->getInteger("import_map_count"), 2);
  const auto ID = field(Summary, "html_id");
  const auto Maps = records(ID, "import_maps", 0, 1);
  ASSERT_EQ(items(Maps).size(), 1u);
  EXPECT_EQ(field(items(Maps)[0], "analysis_status"), "ok");
  EXPECT_EQ(items(Maps)[0].getAsObject()->getBoolean("integrity_present"),
            true);
  EXPECT_EQ(items(Maps)[0].getAsObject()->getBoolean("integrity_verified"),
            false);
  EXPECT_EQ(Maps.getAsObject()->getInteger("next_offset"), 1);
  const auto Inert = records(ID, "import_maps", 1, 1);
  EXPECT_EQ(field(items(Inert)[0], "reason"), "ineligible_html_context");
  EXPECT_EQ(code(records(ID, "import_maps", 3)), "invalid_page");
  EXPECT_EQ(field(analyze(Doc), "import_map_analysis_id"),
            field(Summary, "import_map_analysis_id"));
}

TEST_F(WebHTMLSDK, ImportMapRequestsSeparateURLsAndKeepMappingEvidence) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Requires JS parser";
#else
  const std::string HTML = R"(<script type=importmap>{"imports":{
      "CANARY-a":"./dep.js?CANARY-one","CANARY-b":"./dep.js?CANARY-two",
      "CANARY-same":"./dep.js?CANARY-one","CANARY-stop":null}}</script>
      <script type=module>import 'CANARY-a';import 'CANARY-b';
      import 'CANARY-same';import 'CANARY-stop';</script>)";
  const std::string JS = "export const x=1;";
  write("index.html", HTML);
  write("dep.js", JS);
  capture();
  const auto Target = artifact(JS);
  const auto Summary = analyze(artifact(HTML));
  const auto ID = field(Summary, "html_id");
  const auto Maps = records(ID, "import_maps"), Scripts = records(ID);
  const auto Source =
      field(source(field(items(Scripts)[1], "inline_artifact_id"), "module"),
            "source_id");
  const auto Requests = moduleRequests(Source);
  ASSERT_EQ(items(Requests).size(), 4u);
  const auto &A = items(Requests)[0], &B = items(Requests)[1],
             &Same = items(Requests)[2], &Blocked = items(Requests)[3];
  for (unsigned I = 0; I < 3; ++I) {
    EXPECT_EQ(field(items(Requests)[I], "candidate_artifact_id"), Target);
    EXPECT_EQ(field(items(Requests)[I], "import_map_id"),
              field(items(Maps)[0], "import_map_id"));
  }
  EXPECT_NE(field(A, "module_url_candidate_id"),
            field(B, "module_url_candidate_id"));
  EXPECT_EQ(field(A, "module_url_candidate_id"),
            field(Same, "module_url_candidate_id"));
  EXPECT_EQ(field(Blocked, "link_status"), "blocked_import_map");
  EXPECT_TRUE(Blocked.getAsObject()->get("candidate_artifact_id")->getAsNull());
  cli({"html", Input.string(), "2"}, 0, "\"record_kind\":\"import_maps\"");
  cli({"html-modules", Input.string(), "2", "1"}, 0, "blocked_import_map");
#endif
}

TEST_F(WebHTMLSDK, ExhaustedImportMapsCannotReportSuccessfulCLIComparison) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Requires JS parser";
#else
  std::string HTML;
  for (unsigned I = 0; I < 65; ++I)
    HTML += "<script type=importmap>{}</script>";
  HTML += "<script type=module>import './dep.js';</script>";
  write("index.html", HTML);
  write("dep.js", "1;");
  cli({"html", Input.string(), "2"}, 1, "budget_exceeded");
  cli({"html-modules", Input.string(), "2", "65"}, 1, "budget_exceeded");
#endif
}

TEST_F(WebHTMLSDK, ImportMapTargetsRetainASAROccurrenceIdentity) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Requires JS parser";
#else
  if (!has("asar_extract"))
    GTEST_SKIP() << "ASAR unavailable";
  const std::string HTML =
      R"(<script type=importmap>{"imports":{"CANARY-p":"./b.js"}}
      </script><script type=module>import 'CANARY-p';</script>)";
  const std::string JS = "export default 'CANARY';";
  const auto Archive = asarArchive(
      {{"a.html", asarFile(HTML)}, {"b.js", asarFile(JS, HTML.size())}},
      HTML + JS);
  write("app.asar", Archive);
  write("b.js", JS);
  capture();
  const auto AID = artifact(Archive), Original = artifact(JS);
  const auto Extraction =
      take(neverd_web_asar_extract_json(Web, Revision.data(), Revision.size(),
                                        AID.data(), AID.size(), nullptr, 0));
  const auto EID = field(Extraction, "extraction_id");
  const auto Members = take(neverd_web_asar_records_json(
      Web, Revision.data(), Revision.size(), EID.data(), EID.size(), 0, 512));
  ASSERT_EQ(items(Members).size(), 2u);
  const auto H = analyze(field(items(Members)[0], "member_id"));
  const auto Scripts = records(field(H, "html_id"));
  const auto SID =
      field(source(field(items(Scripts)[1], "inline_artifact_id"), "module"),
            "source_id");
  const auto Requests = moduleRequests(SID);
  ASSERT_EQ(items(Requests).size(), 1u);
  EXPECT_EQ(field(items(Requests)[0], "candidate_artifact_id"),
            field(items(Members)[1], "member_id"));
  EXPECT_NE(field(items(Requests)[0], "candidate_artifact_id"), Original);
  const auto Location = anchor(SID, 0, 1);
  EXPECT_EQ(Location.getAsObject()->getObject("storage")->getString("kind"),
            "html_inline_script");
#endif
}

TEST_F(WebHTMLSDK, PackedHTMLKeepsNestedStorageOffsetsAndMemberNamespace) {
  if (!has("asar_extract"))
    GTEST_SKIP() << "ASAR unavailable";
  const std::string HTML =
      "<script>const x='CANARY';import('./b.js');</script><script "
      "src=b.js></script>";
  const std::string JS = "throw Error('CANARY');";
  const auto Archive = asarArchive(
      {{"a.html", asarFile(HTML)}, {"b.js", asarFile(JS, HTML.size())}},
      HTML + JS);
  write("archive.asar", Archive);
  write("b.js", JS);
  capture();
  const auto OriginalTarget = artifact(JS), AID = artifact(Archive);
  const auto Extraction =
      take(neverd_web_asar_extract_json(Web, Revision.data(), Revision.size(),
                                        AID.data(), AID.size(), nullptr, 0));
  const auto EID = field(Extraction, "extraction_id");
  const auto Members = take(neverd_web_asar_records_json(
      Web, Revision.data(), Revision.size(), EID.data(), EID.size(), 0, 512));
  ASSERT_EQ(items(Members).size(), 2u);
  const auto HTMLMember = field(items(Members)[0], "member_id");
  const auto Analysis = analyze(HTMLMember),
             Page = records(field(Analysis, "html_id"));
  ASSERT_EQ(items(Page).size(), 2u);
  EXPECT_EQ(field(items(Page)[1], "candidate_artifact_id"),
            field(items(Members)[1], "member_id"));
  EXPECT_NE(field(items(Page)[1], "candidate_artifact_id"), OriginalTarget);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Source =
      source(field(items(Page)[0], "inline_artifact_id"), "script");
  EXPECT_EQ(field(Source, "parse_status"), "parsed");
  const auto Linked = moduleRequests(field(Source, "source_id"));
  ASSERT_EQ(items(Linked).size(), 1u);
  EXPECT_EQ(field(items(Linked)[0], "candidate_artifact_id"),
            field(items(Members)[1], "member_id"));
  EXPECT_NE(field(items(Linked)[0], "candidate_artifact_id"), OriginalTarget);
  const auto Location = anchor(field(Source, "source_id"), 0, 5);
  const auto *Storage = Location.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("byte_offset"),
            std::to_string(Archive.find("<script>") + 8));
  EXPECT_EQ(Storage->getObject("parent_origin")->getString("kind"),
            "asar_packed_member");
  EXPECT_EQ(Storage->getObject("parent_origin")->getString("member_id"),
            HTMLMember);
#endif
}

TEST_F(WebHTMLSDK, UnpackedHTMLNeedsAnExplicitCapturedAssociation) {
  if (!has("asar_extract"))
    GTEST_SKIP() << "ASAR unavailable";
  const std::string HTML =
      "<script>const x='CANARY';import('./b.js');</script><script "
      "src=b.js></script>";
  const std::string JS = "0;";
  const auto Archive = asarArchive(
      {{"a.html", asarFile(HTML, 0, true)}, {"b.js", asarFile(JS, 0, true)}});
  write("archive.asar", Archive);
  write("unpacked/a.html", HTML);
  write("unpacked/b.js", JS);
  capture();
  const auto AID = artifact(Archive), OriginalHTML = artifact(HTML);
  const auto OriginalTarget = artifact(JS);
  const auto DirectoryPage = take(
      neverd_web_artifacts_json(Web, Revision.data(), Revision.size(), 2, 1));
  const auto Directory = field(items(DirectoryPage)[0], "artifact_id");
  const auto Extract = [&](std::string_view Unpacked) {
    const auto E = take(neverd_web_asar_extract_json(
        Web, Revision.data(), Revision.size(), AID.data(), AID.size(),
        Unpacked.data(), Unpacked.size()));
    const auto ID = field(E, "extraction_id");
    return take(neverd_web_asar_records_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 512));
  };
  const auto Missing = Extract({});
  ASSERT_EQ(items(Missing).size(), 2u);
  EXPECT_EQ(code(analyze(field(items(Missing)[0], "member_id"))),
            "artifact_bytes_unavailable");
  const auto Members = Extract(Directory);
  const auto Analysis = analyze(field(items(Members)[0], "member_id"));
  const auto Page = records(field(Analysis, "html_id"));
  ASSERT_EQ(items(Page).size(), 2u);
  EXPECT_EQ(field(items(Page)[1], "candidate_artifact_id"),
            field(items(Members)[1], "member_id"));
  EXPECT_NE(field(items(Page)[1], "candidate_artifact_id"), OriginalTarget);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Source =
      source(field(items(Page)[0], "inline_artifact_id"), "script");
  const auto Linked = moduleRequests(field(Source, "source_id"));
  ASSERT_EQ(items(Linked).size(), 1u);
  EXPECT_EQ(field(items(Linked)[0], "candidate_artifact_id"),
            field(items(Members)[1], "member_id"));
  EXPECT_NE(field(items(Linked)[0], "candidate_artifact_id"), OriginalTarget);
  const auto Location = anchor(field(Source, "source_id"), 0, 5);
  const auto *Storage = Location.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("byte_offset"), "8");
  const auto *Parent = Storage->getObject("parent_origin");
  ASSERT_NE(Parent, nullptr);
  EXPECT_EQ(Parent->getString("kind"), "asar_unpacked_member");
  EXPECT_EQ(Parent->getString("storage_artifact_id"), OriginalHTML);
#endif
  cli({"asar-html", Input.string(), "1", "2", "0"}, 0,
      "exact_admitted_file_candidate");
}

TEST_F(WebHTMLSDK, CLIOriginalAndAsarFormsUseOnlyTheirNativeBackend) {
  const std::string HTML =
      "<script>const x='CANARY';import('./b.js');</script><script "
      "src=b.js></script>";
  write("a.html", HTML);
  write("b.js", "0;");
  cli({"html", Input.string(), "1"}, 0, "exact_admitted_file_candidate");
  cli({"html", (Input / "a.html").string(), "0"}, 0,
      "directory_origin_unavailable");
#if NEVERD_TEST_WEB_JAVASCRIPT
  cli({"html-source", Input.string(), "1", "0"}, 0, "parsed");
  cli({"html-source", Input.string(), "1", "1"}, 0, "parsed");
  cli({"html-modules", Input.string(), "1", "0"}, 0,
      "\"kind\":\"dynamic_import\"");
#else
  cli({"html-source", Input.string(), "1", "0"}, 1, "capability_unavailable");
  cli({"html-modules", Input.string(), "1", "0"}, 1, "capability_unavailable");
#endif
  if (!has("asar_extract"))
    return;
  const auto Archive = asarArchive(
      {{"a.html", asarFile(HTML)}, {"b.js", asarFile("0;", HTML.size())}},
      HTML + "0;");
  write("archive.asar", Archive);
  cli({"asar-html", (Input / "archive.asar").string(), "0", "-", "0"}, 0,
      "exact_admitted_file_candidate");
#if NEVERD_TEST_WEB_JAVASCRIPT
  cli({"asar-html-source", (Input / "archive.asar").string(), "0", "-", "0",
       "0"},
      0, "parsed");
  cli({"asar-html-modules", (Input / "archive.asar").string(), "0", "-", "0",
       "0"},
      0, "\"kind\":\"dynamic_import\"");
#else
  cli({"asar-html-source", (Input / "archive.asar").string(), "0", "-", "0",
       "0"},
      1, "capability_unavailable");
  cli({"asar-html-modules", (Input / "archive.asar").string(), "0", "-", "0",
       "0"},
      1, "capability_unavailable");
#endif
}
} // namespace
