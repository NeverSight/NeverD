//===- SessionSignatureProfileTests.cpp - Target selection through the SDK ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real header loading, automatic pattern selection and public JSON reporting.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "web/NativeFixture.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class SessionSignatureProfile : public testing::Test {
protected:
  std::filesystem::path Directory;
  neverd_session_t Session = nullptr;

  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-profile", Path));
    Directory = Path.str().str();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Error;
    if (!Directory.empty())
      std::filesystem::remove_all(Directory, Error);
  }

  std::string write(const char *Name, const std::string &Data) {
    const auto Path = Directory / Name;
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
    EXPECT_TRUE(Output.good());
    return Path.string();
  }

  llvm::Expected<llvm::json::Value> headers() {
    const char *Text = neverd_headers_json(Session);
    auto Value = llvm::json::parse(Text ? Text : "{}");
    neverd_free_string(Text);
    return Value;
  }
};

TEST_F(SessionSignatureProfile, DriverSelectionAndManualResetAreVisible) {
  auto Bytes = neverd::web::test::nativePE();
  neverd::web::test::nativePut(Bytes, 0x98 + 68, 1, 2); // NATIVE
  neverd::web::test::nativePut(Bytes, 0x96, 0x1022, 2); // system image
  const auto Input = write("driver-renamed.txt", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  auto Value = headers();
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  auto *Profile = Value->getAsObject()->getObject("target_profile");
  ASSERT_NE(Profile, nullptr);
  EXPECT_EQ(Profile->getString("system"), "windows");
  EXPECT_EQ(Profile->getString("domain"), "kernel");
  EXPECT_EQ(Value->getAsObject()->getObject("signature_selection"), nullptr);

  write("tree/pe/x86/64/supplemental-bds.pat", "invalid excluded UI pack\n");
  const auto Manual = write("tree/pe/x86/64/custom.npat",
                            "B82A000000C3 00 0000 0006 :0000 answer\n");
  const auto Tree = (Directory / "tree").string();
  ASSERT_EQ(neverd_auto_apply_signatures(Session, Tree.c_str()), 1);
  Value = headers();
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  auto *Selection = Value->getAsObject()->getObject("signature_selection");
  ASSERT_NE(Selection, nullptr);
  EXPECT_EQ(Selection->getInteger("available_files"), 2);
  EXPECT_EQ(Selection->getInteger("selected_files"), 1);
  EXPECT_EQ(Selection->getInteger("skipped_files"), 1);

  ASSERT_EQ(neverd_apply_signature_file(Session, Manual.c_str()), 1);
  Value = headers();
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(Value->getAsObject()->getObject("signature_selection"), nullptr);
}

TEST_F(SessionSignatureProfile, CandidatesNeverReplaceNamesAcrossFormats) {
  const std::string Inputs[] = {
      neverd::web::test::nativePE(), neverd::web::test::nativeELF(),
      neverd::web::test::nativeELF(true), neverd::web::test::nativeMachO()};
  for (size_t I = 0; I < std::size(Inputs); ++I) {
    SCOPED_TRACE(I);
    const auto Input =
        write(("stripped-" + std::to_string(I) + ".bin").c_str(), Inputs[I]);
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
    const auto Pattern = write(
        "candidate.npat", std::string(I == 2 ? "40058052C0035FD6 00 0000 0008"
                                             : "B82A000000C3 00 0000 0006") +
                              " :0000 fabricated_identity\n");
    ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1);
    const char *Text = neverd_sig_matches_json(Session);
    auto Value = llvm::json::parse(Text ? Text : "[]");
    neverd_free_string(Text);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    ASSERT_NE(Value->getAsArray(), nullptr);
    ASSERT_EQ(Value->getAsArray()->size(), 1u);
    const auto *Row = Value->getAsArray()->front().getAsObject();
    ASSERT_NE(Row, nullptr);
    EXPECT_EQ(Row->getString("name"), "fabricated_identity");
    EXPECT_EQ(Row->getString("auto_name"), "");
    EXPECT_EQ(Row->getString("name_evidence"), "none");
    EXPECT_EQ(Row->getString("candidate_reason"),
              "independent_identity_missing");
    EXPECT_LT(neverd_func_find_by_name(Session, "fabricated_identity"), 0);
    ASSERT_EQ(neverd_session_analyze(Session), 1);
    EXPECT_LT(neverd_func_find_by_name(Session, "fabricated_identity"), 0);
    // A user decision remains explicit and cannot turn into signature proof.
    const auto Entry = neverd_session_entry_addr(Session);
    ASSERT_EQ(neverd_rename_addr(Session, Entry, "user_choice"), 0);
    ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1);
    EXPECT_LT(neverd_func_find_by_name(Session, "fabricated_identity"), 0);
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
    EXPECT_EQ(neverd_sig_match_count(Session), 0);
  }
}

TEST_F(SessionSignatureProfile, ExportEvidenceCertifiesOnlyItsExactAlias) {
  // Extend the inert PE with one export table naming its existing function.
  auto Bytes = neverd::web::test::nativePE();
  using neverd::web::test::nativePut;
  nativePut(Bytes, 0x98 + 240 + 8, 512, 4); // section virtual size
  nativePut(Bytes, 0x98 + 112, 0x1040, 4);  // export directory RVA
  nativePut(Bytes, 0x98 + 116, 0x80, 4);
  nativePut(Bytes, 0x240 + 16, 1, 4); // ordinal base
  nativePut(Bytes, 0x240 + 20, 1, 4); // function count
  nativePut(Bytes, 0x240 + 24, 1, 4); // name count
  nativePut(Bytes, 0x240 + 28, 0x1080, 4);
  nativePut(Bytes, 0x240 + 32, 0x1084, 4);
  nativePut(Bytes, 0x240 + 36, 0x1088, 4);
  nativePut(Bytes, 0x280, 0x1000, 4); // function RVA
  nativePut(Bytes, 0x284, 0x1090, 4); // name RVA
  Bytes.replace(0x290, 14, "runtime_answer");
  const auto Input = write("exported.exe", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  ASSERT_GE(neverd_func_find_by_name(Session, "runtime_answer"), 0);
  const auto Pattern =
      write("aliases.pat",
            "B82A000000C3 00 0000 0006 :0000 guess :0000 runtime_answer\n");
  ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1);
  const char *Text = neverd_sig_matches_json(Session);
  auto Value = llvm::json::parse(Text ? Text : "[]");
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  ASSERT_NE(Value->getAsArray(), nullptr);
  ASSERT_EQ(Value->getAsArray()->size(), 1u);
  const auto *Row = Value->getAsArray()->front().getAsObject();
  ASSERT_NE(Row, nullptr);
  EXPECT_EQ(Row->getString("name"), "guess");
  EXPECT_EQ(Row->getString("auto_name"), "runtime_answer");
  EXPECT_EQ(Row->getString("name_evidence"), "image_symbol");
  EXPECT_EQ(Row->getString("candidate_reason"), "");
  EXPECT_LT(neverd_func_find_by_name(Session, "guess"), 0);
  Text = neverd_call_tree_json(Session, "{\"root\":\"runtime_answer\"}");
  auto Tree = llvm::json::parse(Text ? Text : "{}");
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Tree)) << llvm::toString(Tree.takeError());
  ASSERT_NE(Tree->getAsObject(), nullptr);
  const auto *Nodes = Tree->getAsObject()->getArray("nodes");
  ASSERT_NE(Nodes, nullptr);
  ASSERT_FALSE(Nodes->empty());
  EXPECT_EQ(Nodes->front().getAsObject()->getBoolean("library"), true);
}

TEST_F(SessionSignatureProfile, IncomingContextIsVisibleWithoutAutomaticNames) {
  auto Bytes = neverd::web::test::nativePE();
  neverd::web::test::nativePut(Bytes, 0x98 + 240 + 8, 512, 4);
  const unsigned char Caller[] = {0x55, 0x48, 0x89, 0xe5, 0xe8, 0xf7, 0,
                                  0,    0,    0x90, 0x90, 0x90, 0x90, 0x90,
                                  0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                  0x90, 0x90, 0x90, 0x90, 0x5d, 0xc3};
  Bytes.replace(512, sizeof(Caller), reinterpret_cast<const char *>(Caller),
                sizeof(Caller));
  Bytes.replace(768, 3, "\x33\xc0\xc3", 3);
  const auto Input = write("incoming.exe", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Patterns = write(
      "incoming.npat", "554889E5E8........909090909090909090909090909090905DC3 "
                       "00 0000 001B :0000 parent ^0005 zero\n"
                       "33C0C3 00 0000 0003 !caller :0000 zero\n");
  ASSERT_EQ(neverd_apply_signature_file(Session, Patterns.c_str()), 2);
  const char *Text = neverd_sig_matches_json(Session);
  auto Value = llvm::json::parse(Text ? Text : "[]");
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  ASSERT_NE(Value->getAsArray(), nullptr);
  bool Found = false;
  for (const auto &Entry : *Value->getAsArray()) {
    const auto *Row = Entry.getAsObject();
    ASSERT_NE(Row, nullptr);
    if (Row->getString("name") == "zero") {
      Found = true;
      EXPECT_EQ(Row->getBoolean("caller_confirmed"), true);
      EXPECT_EQ(Row->getBoolean("confirmed"), false);
      EXPECT_EQ(Row->getString("auto_name"), "");
      EXPECT_EQ(Row->getString("name_evidence"), "none");
    }
  }
  EXPECT_TRUE(Found);
  EXPECT_LT(neverd_func_find_by_name(Session, "zero"), 0);
}

TEST_F(SessionSignatureProfile, CandidateJSONPreservesAliasesAndInvalidText) {
  const auto Input = write("json.exe", neverd::web::test::nativePE());
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const std::string Name = "body\"\xff";
  const std::string Alias = "long_alias\\with_\"quotes";
  const auto Patterns = write("json.npat", "B82A000000C3 00 0000 0006 :0000 " +
                                               Name + " :0000 " + Alias + "\n");
  ASSERT_EQ(neverd_apply_signature_file(Session, Patterns.c_str()), 1);
  const char *Text = neverd_sig_matches_json(Session);
  ASSERT_NE(Text, nullptr);
  auto Value = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  ASSERT_NE(Value->getAsArray(), nullptr);
  ASSERT_EQ(Value->getAsArray()->size(), 1u);
  const auto *Row = Value->getAsArray()->front().getAsObject();
  ASSERT_NE(Row, nullptr);
  EXPECT_EQ(Row->getString("name"), llvm::json::fixUTF8(Name));
  ASSERT_NE(Row->getArray("aliases"), nullptr);
  ASSERT_EQ(Row->getArray("aliases")->size(), 1u);
  EXPECT_EQ(Row->getArray("aliases")->front().getAsString(), Alias);
  EXPECT_EQ(Row->getBoolean("caller_confirmed"), false);
  EXPECT_EQ(Row->getString("auto_name"), "");
  EXPECT_EQ(Row->getString("candidate_reason"), "independent_identity_missing");
}

} // namespace
