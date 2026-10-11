//===- PackageArchiveTests.cpp - Archive evidence regressions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Hostile framing, namespace, storage and optional real tarball qualification.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "Internal.h"
#include "PackageArchiveFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/Packages.h"

#include <cstdlib>

namespace {
using namespace neverd::web;
using namespace neverd::web::test;

Artifact artifact(std::string_view Bytes) {
  BlobStore Store(MaxPackageArchiveBytes);
  Store.append(Bytes);
  Store.seal();
  Artifact A;
  A.Content = Store.whole();
  A.BlobHash = sha256(Bytes);
  A.ID = identity("archive-test", {A.BlobHash});
  return A;
}
class WebPackageArchive : public ::testing::Test {
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX storage qualification required";
#endif
    if (!packageArchiveAvailable())
      GTEST_SKIP() << "Pinned archive path policy unavailable";
  }
};

TEST_F(WebPackageArchive, UstarMembersKeepExactBytesAndLinksRemainUnavailable) {
  const auto Bytes = finishTar(
      tarMember("package/", "", '5') +
      tarMember("package/package.json", R"({"name":"inert","main":"cli.js"})") +
      tarMember("package/cli.js", "throw 'DO_NOT_EXECUTE';") +
      tarMember("package/escape", "", '2', "../../outside") +
      tarMember("package/hard", "", '1', "package/cli.js"));
  const auto A = extractPackageArchive(artifact(Bytes), "tar");
  ASSERT_EQ(A.Members.size(), 5);
  EXPECT_EQ(A.ExpandedHash, sha256(Bytes));
  EXPECT_EQ(A.Members[2].Content.read(0, A.Members[2].Size),
            "throw 'DO_NOT_EXECUTE';");
  EXPECT_EQ(A.Members[2].BlobHash, sha256("throw 'DO_NOT_EXECUTE';"));
  EXPECT_FALSE(A.Members[3].available());
  EXPECT_TRUE(A.Members[3].BlobHash.empty());
  const auto S = packageArchiveNamespace(A);
  EXPECT_EQ(S.Artifacts.size(), 4);
  const auto P = analyzePackages(S, A.Members[1].ID, "package-json");
  ASSERT_EQ(P.Entries.size(), 1);
  EXPECT_EQ(P.Entries.front().TargetArtifactID, A.Members[2].ID);
}

TEST_F(WebPackageArchive, GzipCRCSizeSingleStreamAndCompleteTailAreRequired) {
  if (!packageGzipAvailable())
    GTEST_SKIP() << "Native zlib unavailable";
  const auto Tar = finishTar(tarMember("package/a", "exact bytes"));
  const auto Gzip = storedGzip(Tar);
  const auto A = extractPackageArchive(artifact(Gzip), "tgz");
  EXPECT_EQ(A.Original.digest(), sha256(Gzip));
  EXPECT_EQ(A.Expanded.digest(), sha256(Tar));
  EXPECT_EQ(A.Members.front().Content.read(0, 11), "exact bytes");
  for (const auto At : {size_t(0), Gzip.size() - 8, Gzip.size() - 4}) {
    auto Bad = Gzip;
    Bad[At] ^= 1;
    EXPECT_THROW(extractPackageArchive(artifact(Bad), "tgz"), Error);
  }
  EXPECT_THROW(extractPackageArchive(artifact(Gzip + Gzip), "tgz"), Error);
  EXPECT_THROW(
      extractPackageArchive(artifact(Gzip + std::string(1, '\0')), "tgz"),
      Error);
  EXPECT_THROW(
      extractPackageArchive(artifact(Gzip.substr(0, Gzip.size() - 1)), "tgz"),
      Error);
  EXPECT_THROW(extractPackageArchive(artifact(Gzip), "tgz", 1024), Error);
}

TEST_F(WebPackageArchive, PaxPathsApplyOnlyToTheFollowingMember) {
  const auto Long = "package/" + std::string(180, 'p') + ".js";
  const auto Bytes =
      finishTar(tarMember("PaxHeader", pax("path", Long), 'x') +
                tarMember("ignored", "one") + tarMember("package/next", "two"));
  const auto A = extractPackageArchive(artifact(Bytes), "tar");
  ASSERT_EQ(A.Members.size(), 2);
  EXPECT_EQ(A.Members[0].Path, Long);
  EXPECT_EQ(A.Members[1].Path, "package/next");
  auto Header = tarMember("ignored", "abc");
  tarNumber(Header, 124, 12, 0);
  tarChecksum(Header);
  const auto Sized =
      finishTar(tarMember("PaxHeader", pax("size", "3"), 'x') + Header);
  EXPECT_EQ(extractPackageArchive(artifact(Sized), "tar").Members[0].Size, 3);
}

TEST_F(WebPackageArchive, UnsupportedPaxAndAmbiguousStateRefuseWholeArchive) {
  for (const auto &Metadata :
       {pax("path", "../escape"), pax("path", ""), pax("GNU.sparse.map", "0,3"),
        pax("size", "-1"), pax("path", "a") + pax("path", "b"),
        std::string("17 path=a\n")}) {
    const auto Bytes = finishTar(tarMember("PaxHeader", Metadata, 'x') +
                                 tarMember("a", "abc"));
    EXPECT_THROW(extractPackageArchive(artifact(Bytes), "tar"), Error);
  }
  const auto P = tarMember("PaxHeader", pax("path", "a"), 'x');
  EXPECT_THROW(extractPackageArchive(artifact(finishTar(P)), "tar"), Error);
  EXPECT_THROW(extractPackageArchive(
                   artifact(finishTar(P + P + tarMember("a", "b"))), "tar"),
               Error);
  EXPECT_THROW(extractPackageArchive(artifact(finishTar(tarMember(
                                         "PaxHeader", pax("path", "a"), 'g'))),
                                     "tar"),
               Error);
}

TEST_F(WebPackageArchive, UnsafeNamesAndTreeAliasesRefuseInEitherOrder) {
  for (const auto *Name : {"/abs", "../escape", "package/../escape", "a\\b",
                           "C:a", "CON", "a//b", "a/./b"})
    EXPECT_THROW(
        extractPackageArchive(artifact(finishTar(tarMember(Name, "b"))), "tar"),
        Error);
  for (const auto &Pair :
       {std::pair{tarMember("package/a", "x"), tarMember("package/a", "y")},
        std::pair{tarMember("package/A", "x"), tarMember("package/a", "y")},
        std::pair{tarMember("package/\u00e9", "x"),
                  tarMember("package/e\u0301", "y")},
        std::pair{tarMember("package/a", "x"), tarMember("package/a/b", "y")},
        std::pair{tarMember("Package/a", "x"), tarMember("package/b", "y")},
        std::pair{tarMember("package/a", "", '2', "outside"),
                  tarMember("package/a/b", "y")}}) {
    EXPECT_THROW(extractPackageArchive(
                     artifact(finishTar(Pair.first + Pair.second)), "tar"),
                 Error);
    EXPECT_THROW(extractPackageArchive(
                     artifact(finishTar(Pair.second + Pair.first)), "tar"),
                 Error);
  }
}

TEST_F(WebPackageArchive, FramingChecksumTypesPaddingAndTruncationRefuse) {
  const auto Valid = finishTar(tarMember("package/a", "abc"));
  for (const auto At : {size_t(0), size_t(257), size_t(1024), size_t(515)}) {
    auto Bad = Valid;
    Bad[At] ^= 1;
    EXPECT_THROW(extractPackageArchive(artifact(Bad), "tar"), Error);
  }
  for (const auto Size : {0, 511, 512, 1024, 1536, 2047})
    EXPECT_THROW(extractPackageArchive(
                     artifact(std::string_view(Valid).substr(0, Size)), "tar"),
                 Error);
  for (const auto Type : {'3', '4', '6', 'S', 'L', 'K'})
    EXPECT_THROW(extractPackageArchive(
                     artifact(finishTar(tarMember("a", "", Type))), "tar"),
                 Error);
  EXPECT_THROW(extractPackageArchive(
                   artifact(finishTar(tarMember("d", "body", '5'))), "tar"),
               Error);
  auto Big = tarMember("big", "");
  tarNumber(Big, 124, 12, MaxPackageArchiveFileBytes + 1);
  tarChecksum(Big);
  EXPECT_THROW(extractPackageArchive(artifact(finishTar(Big)), "tar"), Error);
  EXPECT_THROW(extractPackageArchive(
                   artifact(finishTar(tarMember("link", "", '2'))), "tar"),
               Error);
}

TEST_F(WebPackageArchive, OriginalAndExpandedBudgetsAreNotMemberOnly) {
  const auto Bytes = finishTar(tarMember("a", "b")) + std::string(4096, '\0');
  EXPECT_THROW(extractPackageArchive(artifact(Bytes), "tar", 2048), Error);
  auto A = artifact(Bytes);
  A.BlobHash = "wrong";
  EXPECT_THROW(extractPackageArchive(A, "tar"), Error);
  EXPECT_THROW(extractPackageArchive(artifact(Bytes), "zip"), Error);
  if (!packageGzipAvailable()) {
    try {
      extractPackageArchive(artifact(storedGzip(Bytes)), "tgz");
      FAIL() << "Omitted gzip decoder accepted compressed bytes";
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), "package_gzip_unavailable");
    }
  }
}

TEST_F(WebPackageArchive,
       DerivedStorageIsPrivateUntilSealedAndSlicesKeepOwner) {
  Blob Retained;
  {
    BlobStore S(16);
    S.append("first");
    EXPECT_THROW(S.whole(), Error);
    S.append("second");
    EXPECT_THROW(S.append("over-budget"), Error);
    S.seal();
    Retained = S.whole().slice(5, 6);
    EXPECT_THROW(S.append("x"), Error);
    EXPECT_THROW(S.seal(), Error);
  }
  EXPECT_EQ(Retained.read(0, 6), "second");
}

TEST_F(WebPackageArchive,
       ClaudeCode21296NPMPayloadMatchesStandaloneWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_NPM_LINUX_X64");
  if (!Path || !packageGzipAvailable())
    GTEST_SKIP()
        << "Pinned npm platform tarball not supplied or zlib unavailable";
  const auto S = capture(Path, Limits{});
  ASSERT_EQ(S.Artifacts.front().Content.size(), 114864541);
  ASSERT_EQ(S.Artifacts.front().BlobHash,
            "eef5a2e2b09a5e7d2ce784d1b3ba335ae7fc6c7f6256c802a0ebf2c21e4fd0e1");
  const auto A = extractPackageArchive(S.Artifacts.front(), "tgz");
  ASSERT_EQ(A.Members.size(), 4);
  unsigned Binaries = 0;
  for (const auto &M : A.Members)
    if (M.Size == 257068216) {
      ++Binaries;
      EXPECT_EQ(
          M.BlobHash,
          "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
      Artifact Binary{M.ID, M.BlobHash, A.ID, M.Path, M.Kind, M.Content, false};
      const auto B = extractBun(Binary);
      EXPECT_EQ(B.Modules.size(), 2589);
      EXPECT_EQ(B.Architecture, "x64");
    }
  EXPECT_EQ(Binaries, 1);
}

TEST_F(WebPackageArchive, ClaudeCode21296NPMWrapperWhenSupplied) {
  const auto *Directory = std::getenv("NEVERD_CLAUDE_CODE_21296_NPM_DIR");
  if (!Directory || !packageGzipAvailable())
    GTEST_SKIP() << "Pinned npm wrapper not supplied or zlib unavailable";
  const auto S =
      capture(std::string(Directory) + "/claude-code-2.1.296.tgz", Limits{});
  ASSERT_EQ(S.Artifacts.front().Content.size(), 29001);
  ASSERT_EQ(S.Artifacts.front().BlobHash,
            "f6c375d51d4c22a7a850e185a0d7ddda85173f89dbbeb3262aa7971e3bd670b5");
  const auto A = extractPackageArchive(S.Artifacts.front(), "tgz");
  ASSERT_EQ(A.Members.size(), 7);
  unsigned Manifests = 0;
  for (const auto &M : A.Members)
    if (M.Path == "package/package.json") {
      ++Manifests;
      const auto P =
          analyzePackages(packageArchiveNamespace(A), M.ID, "package-json");
      EXPECT_EQ(P.Packages.size(), 1);
      EXPECT_EQ(P.Dependencies.size(), 8);
      EXPECT_EQ(P.Scripts.size(), 2);
      ASSERT_EQ(P.Entries.size(), 1);
      EXPECT_FALSE(P.Entries.front().TargetArtifactID.empty());
    }
  EXPECT_EQ(Manifests, 1);
}
} // namespace
