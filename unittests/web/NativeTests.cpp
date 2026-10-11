//===- NativeTests.cpp - Immutable native handoff tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable native handoff tests.
///
//===----------------------------------------------------------------------===//

#include "BunFixture.h"
#include "NativeFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using namespace neverd::web::test;
using llvm::json::Value;
Value take(const char *Owned) {
  EXPECT_NE(Owned, nullptr);
  if (!Owned)
    return nullptr;
  auto Parsed = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  EXPECT_TRUE(bool(Parsed));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return nullptr;
  }
  return std::move(*Parsed);
}
std::string field(const Value &V, const char *Key) {
  if (const auto *O = V.getAsObject())
    if (const auto Text = O->getString(Key))
      return Text->str();
  ADD_FAILURE() << "Missing field " << Key << ": "
                << llvm::formatv("{0}", V).str();
  return {};
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *O = V.getAsObject();
  return O && O->getObject("error")
             ? O->getObject("error")->getString("code").value_or("").str()
             : std::string();
}
std::string text(const char *Owned) {
  EXPECT_NE(Owned, nullptr);
  const std::string Result = Owned ? Owned : "";
  neverd_free_string(Owned);
  return Result;
}
std::string digest(std::string_view Bytes) {
  llvm::SHA256 Hash;
  Hash.update(Bytes);
  return llvm::toHex(Hash.final(), true);
}
struct Native {
  neverd_session_t Handle = nullptr;
  ~Native() { neverd_session_destroy(Handle); }
};
class WebNative : public testing::Test {
protected:
  neverd_web_session_t Web = nullptr;
  fs::path Root;
  std::string Path, Revision, Artifact;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "Windows web input capture is not implemented";
#endif
    Web = neverd_web_session_create();
    ASSERT_NE(Web, nullptr);
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-native-handoff", Dir));
    Root = Dir.str().str();
    Path = (Root / "PRIVATE_NATIVE_PATH.bin").string();
  }
  void TearDown() override {
    neverd_web_session_destroy(Web);
    std::error_code Error;
    fs::remove_all(Root, Error);
  }
  void write(std::string_view Bytes) {
    std::ofstream Out(Path, std::ios::binary);
    Out.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(Out.good());
  }
  void capture(std::string_view Bytes) {
    write(Bytes);
    const auto Preview = take(neverd_web_import_preview_json(
        Web, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    const auto Metadata =
        take(neverd_web_import_commit_json(Web, Token.data(), Token.size()));
    Revision = field(Metadata, "revision");
    const auto Page = take(
        neverd_web_artifacts_json(Web, Revision.data(), Revision.size(), 0, 1));
    ASSERT_NE(Page.getAsObject(), nullptr);
    ASSERT_NE(Page.getAsObject()->getArray("items"), nullptr);
    ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 1U);
    Artifact =
        field((*Page.getAsObject()->getArray("items"))[0], "artifact_id");
  }
  Value open(Native &N, std::string_view ID = {}) {
    if (ID.empty())
      ID = Artifact;
    return take(neverd_web_native_open_json(Web, Revision.data(),
                                            Revision.size(), ID.data(),
                                            ID.size(), &N.Handle));
  }
};

TEST_F(WebNative, FileAndSnapshotLoadersAgreeForELFPEAndThinMachO) {
  for (const auto &Bytes : {nativeELF(), nativeELF(true), nativePE(),
                            nativePELongStub(), nativeMachO()}) {
    capture(Bytes);
    Native Snapshot, File;
    const auto Report = open(Snapshot);
    ASSERT_EQ(field(Report, "status"), "ok");
    ASSERT_NE(Snapshot.Handle, nullptr);
    File.Handle = neverd_session_create();
    ASSERT_NE(File.Handle, nullptr);
    neverd_session_set_debug_info_enabled(File.Handle, 0);
    ASSERT_EQ(neverd_session_load(File.Handle, Path.c_str()), 1)
        << text(neverd_last_error(File.Handle));
    EXPECT_EQ(text(neverd_session_arch_name(Snapshot.Handle)),
              text(neverd_session_arch_name(File.Handle)));
    EXPECT_EQ(text(neverd_session_format_name(Snapshot.Handle)),
              text(neverd_session_format_name(File.Handle)));
    EXPECT_EQ(neverd_session_entry_addr(Snapshot.Handle),
              neverd_session_entry_addr(File.Handle));
    EXPECT_EQ(neverd_session_base_addr(Snapshot.Handle),
              neverd_session_base_addr(File.Handle));
    EXPECT_EQ(neverd_session_section_count(Snapshot.Handle),
              neverd_session_section_count(File.Handle));
    EXPECT_EQ(neverd_session_segment_count(Snapshot.Handle),
              neverd_session_segment_count(File.Handle));
    EXPECT_EQ(neverd_session_file_size(Snapshot.Handle), Bytes.size());
    EXPECT_TRUE(text(neverd_session_file_path(Snapshot.Handle)).empty());
    EXPECT_EQ(field(Report, "blob_sha256"), digest(Bytes));
    EXPECT_EQ(text(neverd_session_input_sha256(Snapshot.Handle)),
              digest(Bytes));
    EXPECT_EQ(field(Report, "pipeline_status"), "not_run");
    const auto Headers = take(neverd_headers_json(Snapshot.Handle));
    EXPECT_EQ(Headers.getAsObject()->getInteger("file_size"), Bytes.size());
    const auto Dashboard = take(neverd_dashboard_json(Snapshot.Handle));
    ASSERT_NE(Dashboard.getAsObject()->getObject("hashes"), nullptr);
    EXPECT_EQ(Dashboard.getAsObject()->getObject("hashes")->getString("sha256"),
              digest(Bytes));
    const auto Serialized = llvm::formatv("{0}", Report).str();
    EXPECT_EQ(Serialized.find("CANARY"), std::string::npos);
    EXPECT_EQ(Serialized.find("PRIVATE_NATIVE_PATH"), std::string::npos);
  }
}

TEST_F(WebNative, CapturedBytesSurviveInputChangeAndWebSessionDestruction) {
  const auto Bytes = nativeELF();
  capture(Bytes);
  write("input replaced after capture");
  Native N;
  auto Report = open(N);
  ASSERT_EQ(field(Report, "status"), "ok");
  EXPECT_EQ(field(Report, "blob_sha256"), digest(Bytes));
  fs::remove(Path);
  neverd_web_session_destroy(Web);
  Web = nullptr;
  unsigned char Read[6]{};
  ASSERT_EQ(neverd_read_bytes(N.Handle, 0x400100, Read, sizeof(Read)), 6);
  EXPECT_EQ(std::string(reinterpret_cast<char *>(Read), sizeof(Read)),
            nativeCode());
  const auto Again = take(neverd_web_native_metadata_json(N.Handle));
  EXPECT_EQ(field(Again, "handoff_id"), field(Report, "handoff_id"));
  const auto Analyzed = take(neverd_web_native_analyze_json(N.Handle));
  ASSERT_EQ(field(Analyzed, "status"), "ok");
  EXPECT_EQ(field(Analyzed, "pipeline_status"), "succeeded");
  EXPECT_EQ(field(Analyzed, "handoff_id"), field(Report, "handoff_id"));
  EXPECT_GT(Analyzed.getAsObject()->getInteger("function_count").value_or(0),
            0);
  const auto Source = text(neverd_decompile(N.Handle, 0x400100));
  EXPECT_NE(Source.find("return"), std::string::npos);
}

TEST_F(WebNative, NoCompanionDiscoveryOrPathDependentPersistence) {
  capture(nativeELF());
  std::ofstream(Path + ".neverd-annotations.json")
      << R"([{"addr":"400100","text":"SIDECAR_CANARY"}])";
  std::ofstream(Path + ".neverd-renames.json")
      << R"([{"addr":"400100","renamed":"SIDECAR_CANARY"}])";
  Native N;
  ASSERT_EQ(field(open(N), "status"), "ok");
  EXPECT_EQ(text(neverd_annotations_json(N.Handle)), "[]");
  EXPECT_EQ(text(neverd_renames_json(N.Handle)), "[]");
  neverd_annotation_set(N.Handle, 0x400100, "private in-memory note");
  EXPECT_NE(neverd_annotations_save(N.Handle), 0);
  EXPECT_NE(neverd_annotations_load(N.Handle), 0);
  EXPECT_NE(neverd_renames_save(N.Handle), 0);
  EXPECT_NE(neverd_renames_load(N.Handle), 0);
  for (const auto Operation :
       {neverd_functions_save, neverd_functions_load, neverd_items_save,
        neverd_items_load, neverd_operand_formats_save,
        neverd_operand_formats_load, neverd_load_options_save}) {
    EXPECT_EQ(Operation(N.Handle), -1);
    EXPECT_EQ(text(neverd_last_error(N.Handle)),
              "operation requires a file-backed session");
  }
  EXPECT_EQ(neverd_patch_from_ir(N.Handle, "invalid", 0, nullptr), 0);
  EXPECT_EQ(text(neverd_last_error(N.Handle)),
            "operation requires a file-backed session");
  EXPECT_EQ(neverd_patch_from_c(N.Handle, "invalid", 0, nullptr), 0);
  EXPECT_EQ(text(neverd_last_error(N.Handle)),
            "operation requires a file-backed session");
  EXPECT_EQ(text(neverd_annotation_get(N.Handle, 0x400100)),
            "private in-memory note");
  // A normal file reload remains an explicit separate action and clears the
  // handoff identity only when its ordinary transaction succeeds.
  EXPECT_EQ(neverd_session_load(N.Handle, "/neverd-missing-native-input"), 0);
  EXPECT_EQ(field(take(neverd_web_native_metadata_json(N.Handle)), "status"),
            "ok");
  neverd_session_set_debug_info_enabled(N.Handle, 0);
  EXPECT_EQ(neverd_session_load(N.Handle, Path.c_str()), 1);
  EXPECT_EQ(code(take(neverd_web_native_metadata_json(N.Handle))),
            "native_handoff_required");
  EXPECT_EQ(text(neverd_session_file_path(N.Handle)), Path);
}

TEST_F(WebNative, RejectsInvalidSelectionsBuffersAndImplicitUniversalSlices) {
  Native N;
  EXPECT_EQ(
      code(take(neverd_web_native_open_json(Web, "1", 1, "x", 1, &N.Handle))),
      "no_project");
  capture(nativeELF());
  EXPECT_EQ(code(take(neverd_web_native_open_json(Web, "0", 1, Artifact.data(),
                                                  Artifact.size(), &N.Handle))),
            "stale_revision");
  EXPECT_EQ(code(take(neverd_web_native_open_json(
                Web, Revision.data(), Revision.size(), Artifact.data(),
                Artifact.size(), nullptr))),
            "invalid_output");
  EXPECT_EQ(
      code(take(neverd_web_native_open_json(
          Web, Revision.data(), Revision.size(), nullptr, 64, &N.Handle))),
      "invalid_buffer");
  EXPECT_EQ(code(open(N, "not-an-admitted-selection")),
            "native_selection_unavailable");
  EXPECT_EQ(code(take(neverd_web_native_metadata_json(nullptr))),
            "invalid_session");
  for (const auto Bytes :
       {std::string("console.log('not native')"), std::string("60016000")}) {
    capture(Bytes);
    EXPECT_EQ(code(open(N)), "unsupported_native_format");
    EXPECT_EQ(N.Handle, nullptr);
  }
  std::string Fat(16, '\0');
  Fat.replace(0, 4, "\xca\xfe\xba\xbe", 4);
  Fat[7] = 1;
  capture(Fat);
  EXPECT_EQ(code(open(N)), "native_slice_selection_required");
  EXPECT_EQ(N.Handle, nullptr);
}

TEST_F(WebNative, DirectoryMembersRetainOccurrenceInsteadOfReopeningTheirPath) {
  const auto Bytes = nativeMachO();
  write(Bytes);
  const auto RootPath = Root.string();
  auto Preview = take(neverd_web_import_preview_json(
      Web, RootPath.data(), RootPath.size(), nullptr, 0));
  const auto Token = field(Preview, "preview_token");
  Revision = field(
      take(neverd_web_import_commit_json(Web, Token.data(), Token.size())),
      "revision");
  const auto Page = take(
      neverd_web_artifacts_json(Web, Revision.data(), Revision.size(), 0, 512));
  const auto *Items = Page.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_EQ(Items->size(), 2U);
  const auto RootID = field((*Items)[0], "artifact_id");
  Artifact = field((*Items)[1], "artifact_id");
  fs::remove(Path);
  Native N, Refused;
  const auto Report = open(N);
  ASSERT_EQ(field(Report, "status"), "ok");
  ASSERT_NE(Report.getAsObject()->getObject("origin"), nullptr);
  EXPECT_EQ(Report.getAsObject()->getObject("origin")->getString(
                "parent_artifact_id"),
            RootID);
  EXPECT_EQ(field(Report, "blob_sha256"), digest(Bytes));
  EXPECT_EQ(code(open(Refused, RootID)), "artifact_has_no_bytes");
}

TEST_F(WebNative,
       PinnedBunCompilerContainerEntersTheNativeLoaderWithoutExecution) {
  const char *Corpus = std::getenv("NEVERD_BUN_142_CORPUS");
  if (!Corpus || !*Corpus)
    GTEST_SKIP() << "Pinned full Bun corpus is not configured";
  Path = (fs::path(Corpus) / "plain.elf").string();
  const auto Preview = take(neverd_web_import_preview_json(
      Web, Path.data(), Path.size(), nullptr, 0));
  const auto Token = field(Preview, "preview_token");
  Revision = field(
      take(neverd_web_import_commit_json(Web, Token.data(), Token.size())),
      "revision");
  const auto Page = take(
      neverd_web_artifacts_json(Web, Revision.data(), Revision.size(), 0, 1));
  ASSERT_NE(Page.getAsObject()->getArray("items"), nullptr);
  ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 1U);
  const auto &Item = (*Page.getAsObject()->getArray("items"))[0];
  Artifact = field(Item, "artifact_id");
  const auto Extraction = take(neverd_web_bun_extract_json(
      Web, Revision.data(), Revision.size(), Artifact.data(), Artifact.size()));
  ASSERT_EQ(field(Extraction, "status"), "ok");
  Native N;
  const auto Report = open(N);
  ASSERT_EQ(field(Report, "status"), "ok");
  EXPECT_EQ(field(Report, "blob_sha256"), field(Item, "blob_sha256"));
  EXPECT_EQ(field(Report, "architecture"), "x86_64");
  EXPECT_EQ(field(Report, "pipeline_status"), "not_run");
  EXPECT_GT(neverd_session_file_size(N.Handle), 8U * 1024 * 1024);
  EXPECT_GT(neverd_session_segment_count(N.Handle), 0);
}

TEST_F(WebNative, TruncatedNativeHeadersNeverPublishAHandle) {
  for (const auto Bytes : {nativeELF(), nativePE(), nativeMachO()})
    for (const auto Length : {0U, 1U, 4U, 16U, 63U, 127U, 191U, 255U}) {
      capture(std::string_view(Bytes).substr(0, Length));
      Native N;
      const auto Result = open(N);
      EXPECT_EQ(field(Result, "status"), "error") << Length;
      EXPECT_EQ(N.Handle, nullptr);
      const auto Serialized = llvm::formatv("{0}", Result).str();
      EXPECT_EQ(Serialized.find("CANARY"), std::string::npos);
      EXPECT_EQ(Serialized.find("PRIVATE_NATIVE_PATH"), std::string::npos);
    }
}

TEST_F(WebNative, BunNativeAssetsKeepTheirContainerAndMemberProvenance) {
  const auto Payload = nativePE();
  capture(BunFixture(true, "opaque map", Payload).Bytes);
  const auto Extraction = take(neverd_web_bun_extract_json(
      Web, Revision.data(), Revision.size(), Artifact.data(), Artifact.size()));
  const auto ID = field(Extraction, "extraction_id");
  const auto Modules = take(
      neverd_web_bun_records_json(Web, Revision.data(), Revision.size(),
                                  ID.data(), ID.size(), "modules", 7, 0, 512));
  const auto *Items = Modules.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_EQ(Items->size(), 3U);
  Native N, Refused;
  const auto Selected = field((*Items)[2], "content_region_id");
  const auto Report = open(N, Selected);
  ASSERT_EQ(field(Report, "status"), "ok");
  EXPECT_EQ(field(Report, "blob_sha256"), digest(Payload));
  const auto *Origin = Report.getAsObject()->getObject("origin");
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->getString("kind"), "bun_asset");
  EXPECT_EQ(Origin->getString("container_artifact_id"), Artifact);
  EXPECT_EQ(Origin->getString("extraction_id"), ID);
  EXPECT_EQ(Origin->getString("region_id"), Selected);
  EXPECT_EQ(Origin->getString("module_id"), field((*Items)[2], "module_id"));
  EXPECT_EQ(Origin->getString("byte_length"), std::to_string(Payload.size()));
  EXPECT_EQ(code(open(Refused, field((*Items)[0], "bytecode_region_id"))),
            "native_selection_unavailable");
  EXPECT_EQ(code(open(Refused, field((*Items)[0], "content_region_id"))),
            "native_selection_unavailable");
  EXPECT_EQ(code(open(Refused, field((*Items)[0], "source_artifact_id"))),
            "native_selection_unavailable");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebNative, NativeCliUsesCppOnlyAndPrintsMetadata) {
  const auto Output = (Root / "stdout").string(),
             Errors = (Root / "stderr").string();
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  for (const auto Command :
       {"native", "native-analyze", "bun-native", "bun-native-analyze"}) {
    const bool Bun = std::string_view(Command).starts_with("bun-");
    write(Bun ? BunFixture(false, "", nativeELF()).Bytes : nativeELF());
    std::vector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web", Command,
                                      Path};
    if (Bun)
      Args.push_back("2");
    std::string Error;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                        Redirects, 30, 0, &Error),
              0)
        << Error;
    std::ifstream In(Output), Err(Errors);
    std::stringstream Text, Diagnostics;
    Text << In.rdbuf();
    Diagnostics << Err.rdbuf();
    EXPECT_EQ(Text.str().find("CANARY"), std::string::npos);
    EXPECT_EQ(Diagnostics.str().find("CANARY"), std::string::npos);
    EXPECT_EQ(Text.str().find("PRIVATE_NATIVE_PATH"), std::string::npos);
    EXPECT_NE(Text.str().find("immutable-native-handoff-v1"),
              std::string::npos);
    EXPECT_NE(Text.str().find(std::string_view(Command).ends_with("analyze")
                                  ? "succeeded"
                                  : "not_run"),
              std::string::npos);
  }
}
#endif
} // namespace
