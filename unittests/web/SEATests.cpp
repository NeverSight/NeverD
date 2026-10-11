//===- SEATests.cpp - Offline SEA reader contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline SEA reader contracts.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "JsonReader.h"
#include "SEAFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/SEA.h"

#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using namespace neverd::web;
using namespace neverd::web::sea_test;
class WebSEA : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-sea", D));
    Root = D.str().str();
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  Artifact input(std::string_view B) {
    const auto Path = Root / "input";
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(B.data(), B.size());
    }
    return capture(Path.string(), Limits{}).Artifacts.at(0);
  }
  void refuse(std::string_view B, const char *Code,
              std::string_view P = SEABlobProfile) {
    try {
      (void)extractSEA(input(B), P);
      FAIL() << Code;
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), Code);
    }
  }
};
TEST_F(WebSEA, ExactSerializationAndCompleteBytePartition) {
  const auto B = blob(13);
  const auto A = input(B);
  const auto E = extractSEA(A, SEABlobProfile);
  EXPECT_EQ(E.AssetCount, 2U);
  EXPECT_FALSE(E.SourceArtifactID.empty());
  uint64_t At = 0;
  for (const auto &R : E.Regions) {
    EXPECT_EQ(R.Offset, At);
    EXPECT_EQ(R.Content.read(0, R.Content.size()),
              B.substr(At, R.Content.size()));
    if (R.Private)
      EXPECT_TRUE(R.BlobHash.empty());
    else
      EXPECT_EQ(R.BlobHash, sha256(B.substr(At, R.Content.size())));
    At += R.Content.size();
  }
  EXPECT_EQ(At, B.size());
  EXPECT_EQ(E.ID, extractSEA(A, SEABlobProfile).ID);
}
TEST_F(WebSEA, SnapshotAndCacheAreSeparateOpaqueEvidence) {
  for (const auto Flags : {0U, 1U, 2U, 3U, 4U, 5U, 8U, 10U, 12U, 13U}) {
    const auto E = extractSEA(input(blob(Flags)), SEABlobProfile);
    EXPECT_EQ(E.SourceArtifactID.empty(), bool(Flags & 2));
    for (const auto &R : E.Regions)
      if (R.Kind == "v8_snapshot" || R.Kind == "v8_code_cache")
        EXPECT_FALSE(R.selectable());
  }
  for (const auto Flags : {6U, 7U, 14U, 15U, 16U, UINT32_MAX})
    refuse(blob(Flags), "sea_unsupported_flags");
}
TEST_F(WebSEA, TruncationsUnknownLayoutsAndTrailingBytesRefuse) {
  const auto B = blob(12);
  for (size_t N = 0; N < B.size(); ++N)
    EXPECT_THROW(extractSEA(input(B.substr(0, N)), SEABlobProfile), Error) << N;
  refuse(B + "x", "sea_trailing_bytes");
  auto Bad = B;
  put(Bad, 0, 0xdead, 4);
  refuse(Bad, "sea_invalid_magic");
  Bad = B;
  put(Bad, 8, UINT64_MAX, 8);
  refuse(Bad, "sea_range_out_of_bounds");
  refuse(B, "sea_unsupported_profile", "node-sea-latest");
}
TEST_F(WebSEA, PrivateKeysAndWorkBudgetsRefuse) {
  std::string B;
  append(B, 0x143da20, 4);
  append(B, 8, 4);
  string(B, "main");
  string(B, "");
  append(B, 2);
  string(B, "same");
  string(B, "one");
  string(B, "same");
  string(B, "two");
  refuse(B, "sea_duplicate_asset_key");
  std::string Long;
  append(Long, 0x143da20, 4);
  append(Long, 0, 4);
  string(Long, std::string(32769, 'x'));
  string(Long, "");
  refuse(Long, "sea_name_budget_exceeded");
  B.clear();
  append(B, 0x143da20, 4);
  append(B, 8, 4);
  string(B, "");
  string(B, "");
  append(B, 4097);
  refuse(B, "sea_asset_budget_exceeded");
  B.resize(B.size() - 8);
  append(B, 33);
  for (unsigned I = 0; I < 33; ++I) {
    string(B, std::string(32767, 'x') + char(I));
    string(B, "");
  }
  refuse(B, "sea_name_budget_exceeded");
}
TEST_F(WebSEA, SixNativeProfilesUseTheSameBlobSemantics) {
  const auto B = blob(12);
  for (const auto ARM : {false, true})
    for (const auto *Format : {"elf", "macho", "pe"}) {
      SCOPED_TRACE(std::string(Format) + (ARM ? " arm64" : " x64"));
      const auto Bytes = std::string_view(Format) == "elf"     ? elf(B, ARM)
                         : std::string_view(Format) == "macho" ? macho(B, ARM)
                                                               : pe(B, ARM);
      const auto P = std::string("node-sea-22.15.0-") + Format +
                     (ARM ? "-arm64-v1" : "-x64-v1");
      const auto E = extractSEA(input(Bytes), P);
      EXPECT_EQ(E.AssetCount, 2U);
      EXPECT_EQ(E.ResourceSize, B.size());
      EXPECT_EQ(E.Architecture, ARM ? "arm64" : "x64");
      for (const auto &R : E.Regions)
        EXPECT_EQ(R.Content.read(0, R.Content.size()),
                  Bytes.substr(R.Offset, R.Content.size()));
      const auto Wrong = std::string("node-sea-22.15.0-") + Format +
                         (ARM ? "-x64-v1" : "-arm64-v1");
      refuse(Bytes, "sea_profile_target_mismatch", Wrong);
    }
}
TEST_F(WebSEA, ELFPrefixesDuplicatesAndMappingsRefuse) {
  const auto P = "node-sea-22.15.0-elf-x64-v1";
  auto B = elf(blob());
  B[4120] = 'X';
  refuse(B, "sea_ambiguous_resource_name", P);
  B = elf(blob());
  put(B, 4096, 4, 4);
  B.replace(4108, 8, "NODE_SEA");
  refuse(B, "sea_ambiguous_resource_name", P);
  B = elf(blob());
  put(B, 56, 3, 2);
  B.replace(176, 56, B.substr(120, 56));
  refuse(B, "sea_duplicate_resource", P);
  B = elf(blob());
  put(B, 160, 1, 8);
  refuse(B, "sea_non_file_backed_note", P);
  B = elf(blob());
  put(B, 136, 0x401001, 8);
  refuse(B, "sea_ambiguous_mapping", P);
  B = elf(blob());
  put(B, 56, 3, 2);
  B.replace(176, 56, B.substr(64, 56));
  refuse(B, "sea_ambiguous_mapping", P);
}
TEST_F(WebSEA, ELFRepeatedWalkHasAnExtractionWideBudget) {
  auto B = elf(blob());
  B.resize(4096 + 12000, '\0');
  put(B, 96, B.size(), 8);
  put(B, 104, B.size(), 8);
  put(B, 152, 12000, 8);
  put(B, 160, 12000, 8);
  B.replace(4096, 12000, std::string(12000, '\0'));
  put(B, 56, 11, 2);
  for (unsigned I = 2; I < 11; ++I)
    B.replace(64 + I * 56, 56, B.substr(120, 56));
  refuse(B, "sea_native_table_budget_exceeded", "node-sea-22.15.0-elf-x64-v1");
}
TEST_F(WebSEA, MachOZeroFillAliasesAndCommandsRefuse) {
  const auto P = "node-sea-22.15.0-macho-x64-v1";
  auto B = macho(blob());
  put(B, 168, 1, 4);
  refuse(B, "sea_invalid_resource_section", P);
  B = macho(blob());
  put(B, 152, 0, 4);
  refuse(B, "sea_invalid_load_mapping", P);
  B = macho(blob());
  put(B, 36, 151, 4);
  refuse(B, "sea_invalid_macho_command", P);
  B = macho(blob());
  put(B, 16, 2, 4);
  put(B, 20, 304, 4);
  B.replace(184, 152, B.substr(32, 152));
  refuse(B, "sea_duplicate_resource", P);
  B = macho(blob());
  put(B, 0, 0xcafebabe, 4);
  refuse(B, "sea_unsupported_container", P);
  B = macho(blob());
  B[49] = 'X';
  refuse(B, "sea_ambiguous_resource_name", P);
}
TEST_F(WebSEA, PEChoicesDuplicatesCyclesAndUnbackedDataRefuse) {
  const auto P = "node-sea-22.15.0-pe-x64-v1";
  auto B = pe(blob());
  put(B, 590, 2, 2);
  put(B, 600, 1, 4);
  put(B, 604, 96, 4);
  refuse(B, "sea_ambiguous_resource_language", P);
  B = pe(blob());
  put(B, 556, 2, 2);
  B.replace(568, 8, B.substr(560, 8));
  refuse(B, "sea_duplicate_resource_key", P);
  B = pe(blob());
  put(B, 564, 0x80000020, 4);
  refuse(B, "sea_invalid_resource_directory", P);
  B = pe(blob());
  put(B, 608, 4096, 4);
  refuse(B, "sea_overlapping_metadata", P);
  B = pe(blob());
  put(B, 408, 512, 4);
  refuse(B, "sea_non_file_backed_resource", P);
  B = pe(blob());
  put(B, 642, 'n', 2);
  refuse(B, "sea_resource_not_found", P);
  B = pe(blob());
  put(B, 526, 2, 2);
  put(B, 536, 2, 4);
  put(B, 540, 0x80000020, 4);
  refuse(B, "sea_unsorted_resource_directory", P);
}

std::string fixture(const std::filesystem::path &Path) {
  std::ifstream F(Path, std::ios::binary);
  if (!F)
    throw std::runtime_error("missing fixture");
  return {std::istreambuf_iterator<char>(F), {}};
}
TEST_F(WebSEA, OfficialNodeCompilerBlobsMatchIndependentSourceAndAssetInputs) {
  const auto Dir = std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) / "sea";
  const auto Manifest = parseBoundedJSON(fixture(Dir / "manifest.json"));
  const auto *Cases = Manifest.getAsObject()->getArray("cases");
  ASSERT_NE(Cases, nullptr);
  for (const auto &Case : *Cases) {
    const auto *C = Case.getAsObject();
    ASSERT_NE(C, nullptr);
    if (!C->getBoolean("redistributed").value_or(false))
      continue;
    const auto Name = C->getString("name")->str();
    SCOPED_TRACE(Name);
    const auto B = fixture(Dir / (Name + ".blob"));
    EXPECT_EQ(sha256(B), C->getString("sha256"));
    EXPECT_EQ(std::to_string(B.size()), C->getString("size"));
    const auto E = extractSEA(input(B), SEABlobProfile);
    EXPECT_EQ(E.Flags, C->getInteger("flags"));
    std::set<std::string> Assets, Expected;
    if (const auto *A = C->getArray("assets"))
      for (const auto &H : *A)
        Expected.insert(H.getAsString()->str());
    unsigned Sources = 0, Caches = 0;
    for (const auto &R : E.Regions) {
      if (R.Kind == "javascript_storage") {
        ++Sources;
        EXPECT_EQ(R.Content.read(0, R.Content.size()), C->getString("source"));
      }
      if (R.Kind == "asset")
        Assets.insert(R.BlobHash);
      if (R.Kind == "v8_code_cache") {
        ++Caches;
        EXPECT_GT(R.Content.size(), 0U);
      }
    }
    EXPECT_EQ(Sources, 1U);
    EXPECT_EQ(Caches, (E.Flags & 4) ? 1U : 0U);
    EXPECT_EQ(Assets, Expected);
  }
}
TEST_F(WebSEA, OptionalOfficialNodeSnapshotRemainsOpaque) {
  const auto *Root = std::getenv("NEVERD_NODE_SEA_22150_CORPUS");
  if (!Root)
    GTEST_SKIP() << "Set NEVERD_NODE_SEA_22150_CORPUS to compiler cases";
  const auto Dir = std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) / "sea";
  const auto Manifest = parseBoundedJSON(fixture(Dir / "manifest.json"));
  const auto B = fixture(std::filesystem::path(Root) / "snapshot.blob");
  for (const auto &C : *Manifest.getAsObject()->getArray("cases"))
    if (C.getAsObject()->getString("name") == "snapshot") {
      EXPECT_EQ(sha256(B), C.getAsObject()->getString("sha256"));
      EXPECT_EQ(std::to_string(B.size()), C.getAsObject()->getString("size"));
    }
  const auto E = extractSEA(input(B), SEABlobProfile);
  EXPECT_TRUE(E.SourceArtifactID.empty());
  EXPECT_EQ(E.Flags, 3U);
  unsigned Snapshots = 0;
  for (const auto &R : E.Regions)
    if (R.Kind == "v8_snapshot") {
      ++Snapshots;
      EXPECT_FALSE(R.selectable());
    }
  EXPECT_EQ(Snapshots, 1U);
}
TEST_F(WebSEA, OptionalSixInjectedOfficialNodeImagesPreserveTheCompilerBlob) {
  const auto *Root = std::getenv("NEVERD_NODE_SEA_22150_IMAGES");
  if (!Root)
    GTEST_SKIP() << "Set NEVERD_NODE_SEA_22150_IMAGES to pinned images";
  const auto Dir = std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) / "sea";
  const auto Manifest = parseBoundedJSON(fixture(Dir / "images.json"));
  const auto *Images = Manifest.getAsObject()->getArray("images");
  ASSERT_NE(Images, nullptr);
  ASSERT_EQ(Images->size(), 6);
  const auto Expected = fixture(Dir / "assets.blob");
  for (const auto &Image : *Images) {
    const auto &M = *Image.getAsObject();
    const auto Name = M.getString("name")->str();
    SCOPED_TRACE(Name);
    const auto A =
        capture((std::filesystem::path(Root) / Name).string(), Limits{})
            .Artifacts.at(0);
    ASSERT_EQ(A.BlobHash, M.getString("sha256"));
    ASSERT_EQ(std::to_string(A.Content.size()), M.getString("size"));
    const auto E = extractSEA(A, M.getString("profile")->str());
    EXPECT_EQ(std::to_string(E.ResourceOffset), M.getString("resource_offset"));
    EXPECT_EQ(std::to_string(E.ResourceSize), M.getString("resource_size"));
    EXPECT_EQ(A.Content.read(E.ResourceOffset, E.ResourceSize), Expected);
    EXPECT_EQ(A.Content.slice(E.ResourceOffset, E.ResourceSize).digest(),
              M.getString("resource_sha256"));
    EXPECT_EQ(E.AssetCount, 2U);
    EXPECT_FALSE(E.SourceArtifactID.empty());
  }
}
} // namespace
