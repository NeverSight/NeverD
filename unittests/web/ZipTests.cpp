//===- ZipTests.cpp - ZIP32 evidence and hostile input tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact payloads, unavailable members, namespace and record integrity.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "Internal.h"
#include "JsonReader.h"
#include "ZipFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/Packages.h"

#include <cstdlib>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::test;
Artifact input(std::string_view Bytes) {
  BlobStore Store(MaxPackageArchiveBytes);
  Store.append(Bytes);
  Store.seal();
  Artifact A;
  A.Content = Store.whole();
  A.BlobHash = sha256(Bytes);
  A.ID = identity("zip-test", {A.BlobHash});
  return A;
}
void refuse(const std::string &Bytes) {
  EXPECT_THROW(extractPackageArchive(input(Bytes), "zip"), Error);
}
class WebZip : public ::testing::Test {
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX storage qualification required";
#endif
    if (!packageArchiveAvailable())
      GTEST_SKIP() << "Pinned path policy unavailable";
  }
};

TEST_F(WebZip, IndependentLibarchive380OutputsKeepPinnedHashesAndContents) {
  for (const auto &[Name, Hash] :
       {std::pair{
            "libarchive-stored.zip",
            "9ca1c0715639ea30a429c83726f5c1395aeaee3352c43fee972d986023b94091"},
        std::pair{"libarchive-deflate.zip",
                  "437b82da0a97f30d964e0f37ff4cff127e81f8f21e9d4418034a33087783"
                  "77a1"}}) {
    if (std::string_view(Name).find("deflate") != std::string_view::npos &&
        !packageGzipAvailable())
      continue;
    std::ifstream File(std::string(NEVERD_WEB_FIXTURE_DIR) + "/zip/" + Name,
                       std::ios::binary);
    ASSERT_TRUE(File.good());
    const std::string Bytes{std::istreambuf_iterator<char>(File), {}};
    ASSERT_EQ(sha256(Bytes), Hash);
    const auto A = extractPackageArchive(input(Bytes), "zip");
    ASSERT_EQ(A.Members.size(), 5);
    EXPECT_EQ(A.Members[1].Path, "main.js");
    EXPECT_EQ(A.Members[1].Content.read(0, A.Members[1].Size),
              "export const answer = 42;\n");
    EXPECT_EQ(A.Members[2].Kind, "directory");
    EXPECT_EQ(A.Members[3].Content.read(0, A.Members[3].Size),
              "<script src='../main.js'></script>\n");
    EXPECT_TRUE(A.Members[4].available());
    EXPECT_EQ(A.Members[4].Size, 0);
  }
}

TEST_F(WebZip, StoredDeflateDescriptorsAndEmptyFilesKeepExactEvidence) {
  for (const uint16_t Method : {0, 8}) {
    if (Method == 8 && !packageGzipAvailable())
      continue;
    for (const uint16_t Flag : {0, 8, 0x800, 0x808})
      for (const bool Signed : {false, true}) {
        const ZipFixture F({{"package/", "", 0, 0, 0040755},
                            {"package/package.json", R"({"main":"cli.js"})",
                             Method, Flag, 0100644, Signed},
                            {"package/cli.js", "throw 'INERT';", Method, Flag,
                             0100755, Signed},
                            {"empty", "", Method, Flag, 0100644, Signed}});
        const auto A = extractPackageArchive(input(F.Bytes), "zip");
        ASSERT_EQ(A.Members.size(), 4);
        EXPECT_EQ(A.Profile, ZipArchiveProfile);
        EXPECT_EQ(A.Original.digest(), sha256(F.Bytes));
        EXPECT_EQ(A.Members[2].Content.read(0, 14), "throw 'INERT';");
        EXPECT_EQ(A.Members[2].BlobHash, sha256("throw 'INERT';"));
        EXPECT_TRUE(A.Members[2].Executable);
        EXPECT_EQ(A.Members[2].StoredOffset, F.Data[2]);
        EXPECT_EQ(A.Members[2].LocalHeaderOffset, F.Local[2]);
        EXPECT_EQ(A.Members[2].StoredFrameSize, F.Local[3] - F.Local[2]);
        EXPECT_TRUE(A.Members[3].available());
        EXPECT_EQ(A.Members[3].BlobHash, sha256(""));
        const auto P = analyzePackages(packageArchiveNamespace(A),
                                       A.Members[1].ID, "package-json");
        ASSERT_EQ(P.Entries.size(), 1);
        EXPECT_EQ(P.Entries[0].TargetArtifactID, A.Members[2].ID);
      }
  }
  EXPECT_TRUE(extractPackageArchive(input(ZipFixture({}).Bytes), "zip")
                  .Members.empty());
}

TEST_F(WebZip, UnavailableMembersHaveNoBytesAndStillOwnTheirNames) {
  const ZipFixture F({{"ok.js", "let x=1;"},
                      {"encrypted", "opaque", 0, 1},
                      {"unknown", "opaque", 12}});
  const auto A = extractPackageArchive(input(F.Bytes), "zip");
  ASSERT_EQ(A.Members.size(), 3);
  EXPECT_EQ(A.Members[1].UnavailableReason, "encrypted");
  EXPECT_EQ(A.Members[2].UnavailableReason, "unsupported_compression");
  for (unsigned I : {1, 2}) {
    EXPECT_FALSE(A.Members[I].available());
    EXPECT_TRUE(A.Members[I].BlobHash.empty());
    EXPECT_EQ(A.Members[I].Content.size(), 0);
    EXPECT_EQ(A.Members[I].StoredOffset, F.Data[I]);
  }
  EXPECT_EQ(packageArchiveNamespace(A).Artifacts.size(), 2);
  refuse(ZipFixture({{"a", "opaque", 0, 1}, {"a/b", "x"}}).Bytes);
  refuse(ZipFixture({{"A", "opaque", 12}, {"a", "x"}}).Bytes);
  if (!packageGzipAvailable()) {
    const auto D =
        extractPackageArchive(input(ZipFixture({{"a", "x", 8}}).Bytes), "zip");
    EXPECT_EQ(D.Members[0].UnavailableReason, "deflate_unavailable");
    EXPECT_FALSE(D.Members[0].available());
  }
}

TEST_F(WebZip, PortableNamesAndSpecialMembersRefuseInBothOrders) {
  for (const auto *Name : {"/abs", "../escape", "a/../b", "C:a", "a\\b", "NUL",
                           "a//b", "a/./b", "a.", "a "})
    refuse(ZipFixture({{Name, "x"}}).Bytes);
  for (const auto &Names :
       {std::pair{"A", "a"}, std::pair{"a", "a/b"},
        std::pair{"Parent/a", "parent/b"}, std::pair{"é", "é"}}) {
    refuse(ZipFixture(
               {{Names.first, "x", 0, 0x800}, {Names.second, "x", 0, 0x800}})
               .Bytes);
    refuse(ZipFixture(
               {{Names.second, "x", 0, 0x800}, {Names.first, "x", 0, 0x800}})
               .Bytes);
  }
  for (const uint32_t Mode : {0120777, 0020600, 0010600, 0140600})
    refuse(ZipFixture({{"link", "outside", 0, 0, Mode}}).Bytes);
  refuse(ZipFixture({{"é", "x"}}).Bytes);
  refuse(ZipFixture({{std::string("a\0b", 3), "x"}}).Bytes);
  refuse(ZipFixture({{"directory/", "not empty", 0, 0, 0040755}}).Bytes);
}

TEST_F(WebZip, EveryTruncationAndConflictingRecordRefuses) {
  const ZipFixture F({{"a.js", "const x=1;"}, {"b.js", "const y=2;"}});
  for (size_t N = 0; N < F.Bytes.size(); ++N)
    refuse(F.Bytes.substr(0, N));
  for (const auto At :
       {size_t(0), F.Local[0] + 4, F.Local[0] + 6, F.Local[0] + 8,
        F.Local[0] + 14, F.Local[0] + 18, F.Local[0] + 22, F.Local[0] + 26,
        F.Local[0] + 30, F.Data[0], F.Central[0], F.Central[0] + 42, F.End + 8,
        F.End + 12, F.End + 16}) {
    auto Bad = F.Bytes;
    Bad[At] ^= 1;
    refuse(Bad);
  }
  refuse(F.Bytes + 'x');
  refuse(std::string("prefix") + F.Bytes);
  auto Overlap = F.Bytes;
  zipPut(Overlap, F.Central[1] + 42, F.Local[0], 4);
  refuse(Overlap);
  auto Zip64 = F.Bytes;
  zipPut(Zip64, F.End + 10, 0xffff, 2);
  zipPut(Zip64, F.End + 8, 0xffff, 2);
  refuse(Zip64);
  auto TwoEnds = F.Bytes;
  zipPut(TwoEnds, F.End + 20, 22, 2);
  TwoEnds += F.Bytes.substr(F.End);
  refuse(TwoEnds);
}

TEST_F(WebZip, DescriptorsFlagsAndExtraFieldsMustAgree) {
  for (const bool Signed : {false, true}) {
    const ZipFixture F({{"a", "abcdef", 0, 8, 0100644, Signed}});
    auto Bad = F.Bytes;
    Bad[F.Data[0] + 6 + (Signed ? 4 : 0)] ^= 1;
    refuse(Bad);
    Bad = F.Bytes;
    zipPut(Bad, 14, 1, 4);
    refuse(Bad);
  }
  for (const uint16_t Flag : {2, 0x10, 0x40, 0x2000, 0x8000})
    refuse(ZipFixture({{"a", "abc", 0, Flag}}).Bytes);
  for (const uint16_t Tag : {1, 0x7075, 0x000d, 0x9901, 0xeeee}) {
    std::string Extra(4, '\0');
    zipPut(Extra, 0, Tag, 2);
    refuse(ZipFixture({{"a", "abc", 0, 0, 0100644, true, Extra}}).Bytes);
  }
  for (const auto &Extra :
       {std::string("x"), std::string("\x55\x54\xff\xff", 4),
        std::string("\x55\x54\0\0\x55\x54\0\0", 8)})
    refuse(ZipFixture({{"a", "abc", 0, 0, 0100644, true, Extra}}).Bytes);
}

TEST_F(WebZip, DeclaredAndActualExpansionAreBoundedBeforePublication) {
  const ZipFixture F({{"a", "abcd"}, {"b", "efgh"}});
  EXPECT_THROW(extractPackageArchive(input(F.Bytes), "zip", 7), Error);
  for (const uint32_t Size : {100000U, 0xffffffffU, 268435457U}) {
    auto Bad = F.Bytes;
    zipPut(Bad, F.Central[0] + 24, Size, 4);
    zipPut(Bad, F.Local[0] + 22, Size, 4);
    refuse(Bad);
  }
  std::string Deep;
  for (unsigned I = 0; I < 65; ++I)
    Deep += "d/";
  refuse(ZipFixture({{Deep + "a", "x"}}).Bytes);
  if (!packageGzipAvailable())
    return;
  const ZipFixture D({{"a", "abcdef", 8}});
  auto Invalid = D.Bytes;
  Invalid[D.Data[0]] = 7; // Reserved deflate block type.
  refuse(Invalid);
  auto Short = D.Bytes;
  zipPut(Short, D.Central[0] + 24, 5, 4);
  zipPut(Short, D.Local[0] + 22, 5, 4);
  refuse(Short);
}

TEST_F(WebZip, MetadataExtensionsValidateTheirInternalFraming) {
  for (const uint16_t Tag : {0x5455, 0x7875, 0x000a}) {
    std::string Empty(4, '\0');
    zipPut(Empty, 0, Tag, 2);
    refuse(ZipFixture({{"a", "x", 0, 0, 0100644, true, Empty}}).Bytes);
  }
  std::string NTFS(36, '\0');
  zipPut(NTFS, 0, 0x000a, 2);
  zipPut(NTFS, 2, 32, 2);
  zipPut(NTFS, 8, 1, 2);
  zipPut(NTFS, 10, 24, 2);
  EXPECT_NO_THROW(extractPackageArchive(
      input(ZipFixture({{"a", "x", 0, 0, 0100644, true, NTFS}}).Bytes), "zip"));
  for (const size_t At : {4, 8, 10}) {
    auto Bad = NTFS;
    Bad[At] ^= 2;
    refuse(ZipFixture({{"a", "x", 0, 0, 0100644, true, Bad}}).Bytes);
  }
  std::string UID("\x75\x78\x05\x00\x01\x01\x00\x01\x00", 9);
  EXPECT_NO_THROW(extractPackageArchive(
      input(ZipFixture({{"a", "x", 0, 0, 0100644, true, UID}}).Bytes), "zip"));
  for (const size_t At : {4, 5, 7}) {
    auto Bad = UID;
    Bad[At] = 9;
    refuse(ZipFixture({{"a", "x", 0, 0, 0100644, true, Bad}}).Bytes);
  }
}

TEST_F(WebZip, ClaudeCode21296VSIXWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_VSIX_LINUX_X64");
  if (!Path)
    GTEST_SKIP() << "Optional pinned Marketplace VSIX not supplied";
  if (!packageZipDeflateAvailable())
    GTEST_SKIP() << "Native deflate unavailable";
  const auto S = capture(Path, Limits{});
  ASSERT_EQ(S.Artifacts.size(), 1);
  ASSERT_EQ(S.Artifacts[0].Content.size(), 118957454);
  ASSERT_EQ(S.Artifacts[0].BlobHash,
            "31176c4a2a29144673170dfb9affc2589af997d01532f1f7f97f8b93999a3d11");
  const auto A = extractPackageArchive(S.Artifacts[0], "zip");
  ASSERT_EQ(A.Members.size(), 29);
  EXPECT_EQ(A.ExpandedBytes, 268754709);
  unsigned Manifests = 0, Helpers = 0;
  for (unsigned I = 0; I < A.Members.size(); ++I) {
    const auto &M = A.Members[I];
    ASSERT_TRUE(M.available());
    if (M.Path == "extension/package.json") {
      ++Manifests;
      const auto J = parseBoundedJSON(M.Content.read(0, M.Size),
                                      {1024 * 1024, 32, 20000, 65536});
      ASSERT_NE(J.getAsObject(), nullptr);
      EXPECT_EQ(J.getAsObject()->getString("version"), "2.1.296");
      const auto P =
          analyzePackages(packageArchiveNamespace(A), M.ID, "package-json");
      EXPECT_FALSE(P.Entries.empty());
    }
    if (M.BlobHash ==
        "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de") {
      ++Helpers;
      EXPECT_EQ(I, 15);
      Artifact Helper;
      Helper.ID = M.ID;
      Helper.Content = M.Content;
      Helper.BlobHash = M.BlobHash;
      const auto B = extractBun(Helper);
      EXPECT_EQ(B.Modules.size(), 2589);
      EXPECT_EQ(B.Regions.size(), 10005);
      std::cout << "Pinned helper member index: " << I << '\n';
    }
  }
  EXPECT_EQ(Manifests, 1);
  EXPECT_EQ(Helpers, 1);
  std::cout << "Pinned VSIX members: " << A.Members.size()
            << "; expanded bytes: " << A.ExpandedBytes << '\n';
}
} // namespace
