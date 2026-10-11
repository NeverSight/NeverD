//===- AsarTests.cpp - Bounded ASAR archive extraction tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded ASAR archive extraction tests.
///
//===----------------------------------------------------------------------===//

#include "AsarFixture.h"
#include "Internal.h"
#include "PathPolicy.h"
#include "gtest/gtest.h"

#include "neverd/web/Asar.h"

#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::test;

class WebAsar : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "Windows snapshot capture is not implemented";
#endif
    if (!asarAvailable())
      GTEST_SKIP() << "Pinned native ICU path policy is unavailable";
    llvm::SmallString<128> P;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-asar", P));
    Root = P.str().str();
  }
  void TearDown() override {
    if (!Root.empty()) {
      std::error_code EC;
      std::filesystem::remove_all(Root, EC);
    }
  }
  void write(std::string_view Name, std::string_view Bytes) {
    const auto Path = Root / std::string(Name);
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream F(Path, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    if (!F)
      throw std::runtime_error("fixture write");
  }
  AsarExtraction extract(std::string_view Bytes) {
    write("app.asar", Bytes);
    auto S = capture((Root / "app.asar").string(), Limits{});
    return extractAsar(S, S.Artifacts[0].ID);
  }
  void refuse(std::string_view Bytes, std::string_view Code) {
    try {
      (void)extract(Bytes);
      FAIL() << "accepted " << Code;
    } catch (const Error &E) {
      EXPECT_EQ(E.what(), Code);
    }
  }
};

std::string preserved(std::string_view Name) {
  std::ifstream F(std::string(NEVERD_WEB_FIXTURE_DIR) + "/asar/" +
                      std::string(Name),
                  std::ios::binary);
  if (!F)
    throw std::runtime_error("missing preserved ASAR");
  return {std::istreambuf_iterator<char>(F), {}};
}
const AsarMember &member(const AsarExtraction &E, std::string_view Path) {
  for (const auto &M : E.Members)
    if (M.Path == Path)
      return M;
  throw std::runtime_error("missing fixture member");
}
const Artifact &artifact(const Snapshot &S, std::string_view Path) {
  for (const auto &A : S.Artifacts)
    if (A.MemberPath == Path)
      return A;
  throw std::runtime_error("missing fixture artifact");
}

TEST_F(WebAsar, PreservedUpstreamWriterOutputsMatchHashesAndIndependentMember) {
  const std::pair<const char *, const char *> Cases[]{
      {"packthis.asar",
       "9f6a1857d060c5f03b3e33504ff5db5010e54b035b7b85ce44cb53742b29c2c2"},
      {"packthis-unpack.asar",
       "50da7f9d94f3d2da4983c4482639a4e7b6125dd29d4706530ee2f59569e85b47"},
      {"all-unpacked.asar",
       "a35ffaa85641c6be846d8715f4e5e34a6b0b268717e1a8aa46db3e40ee7f943f"},
      {"unicode.asar",
       "93cf302c522c2307a6f23a873acf05d60810d5cdaeb4d64ec1b95fe3c29a50a9"},
      {"prototype.asar",
       "cbfc2c29b4e661c69f1aaf0f4192d2cc266a6a16cd81faf215a3c5a561a6b593"}};
  for (const auto &[Name, Hash] : Cases) {
    SCOPED_TRACE(Name);
    const auto Bytes = preserved(Name);
    ASSERT_EQ(sha256(Bytes), Hash);
    const auto E = extract(Bytes);
    EXPECT_FALSE(E.Members.empty());
    for (const auto &M : E.Members)
      if (M.available()) {
        EXPECT_EQ(M.IntegrityStatus, "verified_bytes");
        EXPECT_EQ(M.BlobHash, M.Content.digest());
      }
    if (std::string_view(Name) == "packthis.asar") {
      const auto Truth = preserved("unpacked-file.bin");
      ASSERT_EQ(
          sha256(Truth),
          "cc402b796dc92b2b1f3a6d09515003d8400e63d8acaffc967e49c0cf015fcffe");
      const auto &M = member(E, "dir2/file2.png");
      EXPECT_EQ(M.Content.read(0, M.Size), Truth);
      EXPECT_EQ(M.BlobHash, sha256(Truth));
    }
    if (std::string_view(Name) == "unicode.asar")
      EXPECT_TRUE(member(E, "dir1/女の子.txt").available());
  }
}

TEST_F(WebAsar, PackedBytesHaveStableIdentityExactOriginsAndOwnedLifetime) {
  const std::string JS = "export const x = 7;";
  const auto Bytes = asarArchive(
      {{"x.js", asarFile(JS)}, {"empty", asarFile({}, JS.size())}}, JS);
  const auto E = extract(Bytes), Again = extract(Bytes);
  EXPECT_EQ(E.ID, Again.ID);
  const auto &M = member(E, "x.js");
  EXPECT_EQ(M.Offset, E.DataOffset);
  EXPECT_EQ(Bytes.substr(M.Offset, M.Size), JS);
  EXPECT_EQ(M.IntegrityStatus, "verified_bytes");
  std::filesystem::remove(Root / "app.asar");
  EXPECT_EQ(M.Content.read(0, M.Size), JS);
  EXPECT_EQ(member(E, "empty").BlobHash, sha256(""));
  EXPECT_EQ(E.UnreferencedPayloadBytes, 0);
  const auto WithTrailer = extract(Bytes + "unclaimed");
  EXPECT_EQ(WithTrailer.UnreferencedPayloadBytes, 9);
  EXPECT_EQ(member(WithTrailer, "x.js").BlobHash, M.BlobHash);
}

TEST_F(WebAsar, UnpackedAssociationUsesOnlySelectedCapturedOccurrences) {
  const auto Bytes = preserved("packthis-unpack.asar");
  const auto Truth = preserved("unpacked-file.bin");
  write("app.asar", Bytes);
  write("chosen/dir2/file2.png", Truth);
  write("app.asar.unpacked/dir2/file2.png", "wrong companion");
  auto S = capture(Root.string(), Limits{});
  const auto &A = artifact(S, "app.asar");
  const auto &Dir = artifact(S, "chosen");
  const auto Missing = extractAsar(S, A.ID);
  EXPECT_EQ(member(Missing, "dir2/file2.png").Status, "unpacked_not_selected");
  const auto E = extractAsar(S, A.ID, Dir.ID);
  const auto &M = member(E, "dir2/file2.png");
  EXPECT_NE(E.ID, Missing.ID);
  EXPECT_TRUE(M.available());
  EXPECT_EQ(M.Offset, 0);
  EXPECT_EQ(M.StorageArtifactID, artifact(S, "chosen/dir2/file2.png").ID);
  EXPECT_EQ(M.Content.read(0, M.Size), Truth);
  std::filesystem::remove_all(Root / "chosen");
  EXPECT_EQ(M.Content.read(0, M.Size), Truth);
  EXPECT_THROW(extractAsar(S, A.ID, A.ID), Error);
}

TEST_F(WebAsar, MissingWrongSizedAndMismatchedExternalBytesCannotBeConsumed) {
  const auto Bytes = asarArchive({{"one", asarFile("abc", 0, true)},
                                  {"two", asarFile("xyz", 0, true)},
                                  {"three", asarFile("abc", 0, true)},
                                  {"four", asarFile("abc", 0, true)}});
  write("app.asar", Bytes);
  write("selected/one", "a");
  write("selected/two", "bad");
  write("selected/four/child", "abc");
  auto S = capture(Root.string(), Limits{});
  const auto E =
      extractAsar(S, artifact(S, "app.asar").ID, artifact(S, "selected").ID);
  EXPECT_EQ(member(E, "one").Status, "size_mismatch");
  EXPECT_EQ(member(E, "two").Status, "integrity_mismatch");
  EXPECT_EQ(member(E, "three").Status, "missing_member");
  EXPECT_EQ(member(E, "four").Status, "member_type_mismatch");
  for (const auto &M : E.Members) {
    EXPECT_FALSE(M.available());
    EXPECT_EQ(M.Content.size(), 0);
  }
}

TEST_F(WebAsar, HeaderTruncationPaddingAndCheckedRangesRejectAtomically) {
  const auto Full = asarArchive({{"a", asarFile("abc")}}, "abc");
  for (size_t N = 0; N < Full.size(); ++N) {
    SCOPED_TRACE(N);
    EXPECT_THROW(extract(std::string_view(Full).substr(0, N)), Error);
  }
  auto Bad = Full;
  asarU32(Bad, 0, 8);
  refuse(Bad, "asar_invalid_pickle");
  Bad = asarBytes("{}");
  Bad[18] = 1;
  refuse(Bad, "asar_invalid_padding");
  for (const auto Offset : {"-1", "00", "1x", "18446744073709551616"})
    refuse(asarArchive(
               {{"a", llvm::json::Object{{"size", 0}, {"offset", Offset}}}}),
           "asar_invalid_offset");
  refuse(asarArchive(
             {{"a", llvm::json::Object{{"size", 1},
                                       {"offset", "18446744073709551615"}}}}),
         "asar_range_out_of_bounds");
  refuse(asarArchive({{"a", asarFile("abc")}, {"b", asarFile("bc", 1)}}, "abc"),
         "asar_overlapping_members");
}

TEST_F(WebAsar, PrivatePathValidationCoversUnicodeAliasesDevicesAndEscapes) {
  const auto Empty = asarFile("");
  for (const auto Name :
       {"../x", "/root", "C:x", "a\\b", "x.", "x ", "COM¹", "LPT².txt"})
    refuse(asarArchive({{Name, llvm::json::Object(Empty)}}),
           "unsafe_member_name");
  for (const auto &[A, B] :
       std::initializer_list<std::pair<const char *, const char *>>{
           {"é", "é"}, {"가", "가"}, {"Straße", "STRASSE"}, {"A", "a"}})
    refuse(asarArchive({{A, llvm::json::Object(Empty)},
                        {B, llvm::json::Object(Empty)}}),
           "asar_member_collision");
  EXPECT_EQ(archivePathKey("dir/É"), archivePathKey("DIR/é"));
  EXPECT_THROW(archivePathKey("dir/"), Error);
  EXPECT_THROW(archivePathKey("dir/../a"), Error);
  // JSON parser must compare decoded property keys, not source spellings.
  EXPECT_THROW(
      extract(asarBytes(
          R"({"files":{"a":{"size":0,"offset":"0"},"\u0061":{"size":0,"offset":"0"}}})")),
      Error);
}

TEST_F(WebAsar, LinksAndUnknownIntegrityRemainVisibleWithoutUsableBytes) {
  auto Unknown = asarFile("abc");
  Unknown["integrity"] = llvm::json::Object{{"algorithm", "future"}};
  const auto E = extract(asarArchive(
      {{"a", std::move(Unknown)}, {"l", llvm::json::Object{{"link", "a"}}}},
      "abc"));
  EXPECT_EQ(member(E, "a").IntegrityStatus, "unsupported_algorithm");
  EXPECT_FALSE(member(E, "a").available());
  EXPECT_EQ(member(E, "l").Status, "link_not_followed");
  refuse(asarArchive({{"l", llvm::json::Object{{"link", "../escape"}}}}),
         "unsafe_member_name");
}

TEST_F(WebAsar, IntegrityChecksEveryBlockIncludingEmptyTerminalBlocks) {
  for (const auto Data : {"", "abc", "abcd", "abcde", "abcdefgh"}) {
    auto F = asarFile(Data);
    F["integrity"] = asarIntegrity(Data, 4);
    const auto E = extract(asarArchive({{"a", llvm::json::Object(F)}}, Data));
    EXPECT_EQ(member(E, "a").IntegrityStatus, "verified_bytes");
    auto *I = F.getObject("integrity");
    (*I->getArray("blocks"))[0] = std::string(64, '0');
    const auto Bad = extract(asarArchive({{"a", std::move(F)}}, Data));
    EXPECT_EQ(member(Bad, "a").Status, "integrity_mismatch");
    EXPECT_EQ(member(Bad, "a").Content.size(), 0);
  }
  auto Bad = asarFile("abcd");
  Bad["integrity"] = asarIntegrity("abcd", 4);
  Bad.getObject("integrity")->getArray("blocks")->pop_back();
  refuse(asarArchive({{"a", std::move(Bad)}}, "abcd"),
         "asar_invalid_integrity");
}

TEST_F(WebAsar, CountDepthAndMetadataVariantsHaveExplicitLimits) {
  auto OversizedHeader = asarBytes("{}");
  asarU32(OversizedHeader, 4, MaxAsarHeaderBytes + 4);
  asarU32(OversizedHeader, 8, MaxAsarHeaderBytes);
  asarU32(OversizedHeader, 12, MaxAsarHeaderBytes - 4);
  refuse(OversizedHeader, "asar_header_budget_exceeded");
  refuse(asarArchive({{"large",
                       llvm::json::Object{{"size", Limits::HardMemberBytes + 1},
                                          {"unpacked", true}}}}),
         "asar_payload_budget_exceeded");
  refuse(
      asarArchive({{"a", llvm::json::Object{{"size", Limits::HardMemberBytes},
                                            {"unpacked", true}}},
                   {"b", llvm::json::Object{{"size", Limits::HardMemberBytes},
                                            {"unpacked", true}}},
                   {"c", llvm::json::Object{{"size", 1}, {"unpacked", true}}}}),
      "asar_payload_budget_exceeded");
  llvm::json::Object Files;
  for (uint64_t I = 0; I <= MaxAsarMembers; ++I)
    Files[std::to_string(I)] = llvm::json::Object{{"offset", "0"}, {"size", 0}};
  refuse(asarArchive(std::move(Files)), "asar_member_budget_exceeded");
  llvm::json::Object Deep;
  for (uint64_t I = 0; I <= MaxAsarDepth; ++I)
    Deep = llvm::json::Object{
        {"dir", llvm::json::Object{{"files", std::move(Deep)}}}};
  refuse(asarArchive(std::move(Deep)), "asar_depth_budget_exceeded");
  refuse(asarArchive({{"bad", llvm::json::Object{{"size", 0},
                                                 {"offset", "0"},
                                                 {"unpacked", true}}}}),
         "asar_conflicting_storage");
  refuse(asarArchive({{"bad", llvm::json::Object{{"size", 0},
                                                 {"offset", "0"},
                                                 {"unknown", true}}}}),
         "asar_unsupported_metadata");
}

TEST_F(WebAsar, MissingIntegrityAndInheritedUnpackedRemainDistinct) {
  const auto Packed = asarArchive(
      {{"a", llvm::json::Object{{"size", 3}, {"offset", "0"}}}}, "abc");
  const auto E = extract(Packed);
  EXPECT_TRUE(member(E, "a").available());
  EXPECT_EQ(member(E, "a").IntegrityStatus, "missing");
  EXPECT_EQ(member(E, "a").BlobHash, asarHash("abc"));
  const auto Directory = asarArchive(
      {{"dir", llvm::json::Object{
                   {"unpacked", true},
                   {"files", llvm::json::Object{
                                 {"a", llvm::json::Object{{"size", 3}}}}}}}});
  write("a.asar", Directory);
  write("chosen/dir/a", "abc");
  const auto S = capture(Root.string(), Limits{});
  const auto External =
      extractAsar(S, artifact(S, "a.asar").ID, artifact(S, "chosen").ID);
  EXPECT_TRUE(member(External, "dir/a").available());
  EXPECT_TRUE(member(External, "dir/a").Unpacked);
  EXPECT_EQ(member(External, "dir/a").IntegrityStatus, "missing");
  refuse(asarArchive(
             {{"dir",
               llvm::json::Object{
                   {"unpacked", true},
                   {"files",
                    llvm::json::Object{
                        {"a", llvm::json::Object{{"size", 0},
                                                 {"offset", "0"},
                                                 {"unpacked", false}}}}}}}}),
         "asar_conflicting_storage");
}
TEST_F(WebAsar, SelectedUnpackedIndexSharesTheCumulativePathBudget) {
  // Build an admitted-model fixture without asking the host filesystem to
  // represent thousands of near-limit nested paths.
  write("a.asar", asarArchive(llvm::json::Object{}));
  auto S = capture((Root / "a.asar").string(), Limits{});
  const auto ArchiveID = S.Artifacts[0].ID;
  Artifact Directory;
  Directory.ID = "selected-directory";
  Directory.Directory = true;
  Directory.MemberPath = "chosen";
  S.Artifacts.push_back(Directory);
  std::string Prefix;
  for (unsigned I = 0; I < 19; ++I)
    Prefix += std::string(200, 'a') + '/';
  const auto Count = MaxAsarPathBytes / Prefix.size() + 1;
  ASSERT_LT(Count, MaxAsarMembers);
  for (uint64_t I = 0; I < Count; ++I) {
    Artifact A;
    A.ID = std::to_string(I);
    A.ParentID = Directory.ID;
    A.MemberPath = Directory.MemberPath + '/' + Prefix + std::to_string(I);
    S.Artifacts.push_back(std::move(A));
  }
  try {
    (void)extractAsar(S, ArchiveID, Directory.ID);
    FAIL() << "Unpacked index escaped its cumulative path budget";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), "asar_path_budget_exceeded");
  }
}
} // namespace
