//===- ClaudeDistributionTests.cpp - Pinned native npm distributions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Optional captured Claude Code 2.1.296 archives qualify exact container
/// ranges and strict source decoding. Targets and installers remain inert.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/PackageIntegrity.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace {
using namespace neverd::web;
struct Distribution {
  const char *Target, *Format, *Platform, *Architecture;
  const char *ArchiveHash, *ExecutableHash;
  uint64_t ArchiveBytes, ExecutableBytes, GraphOffset, GraphBytes;
  uint32_t Modules, Regions, Flags;
  uint64_t Sources, SourceBytes;
};
// Exact retained npm release bytes, captured 2026-10-12. Their SHA-512
// declarations are compared separately; no publisher authentication is claimed.
constexpr Distribution Cases[] = {
    {"linux-x64", "elf", "linux", "x64",
     "eef5a2e2b09a5e7d2ce784d1b3ba335ae7fc6c7f6256c802a0ebf2c21e4fd0e1",
     "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de",
     114864541, 257068216, 89120776, 167890143, 2589, 10006, 7167, 2345,
     44768763},
    {"linux-arm64", "elf", "linux", "arm64",
     "56abbe1cfc85e5b37afeef5ae8b4856ddc32bbe443b82689a3b5f99992045cd1",
     "f1f6e96e0d8342b9dbf41d7e88255397a6a52ce3d8736ad6a4c6b59c9b62fefa",
     115122331, 256434168, 88604680, 167748717, 2589, 10006, 8191, 2345,
     44760184},
    {"darwin-x64", "macho", "macos", "x64",
     "ec29d962f0f7c70cf8c44606ee7fbb3cd7936a33f4dc13d3c2e3664c5f0b3fc4",
     "a6bf4f30be241053a923f3820ad23c5991d2f5ac2935b3a8dd64da218d9cc603",
     109248203, 249194752, 75231240, 171760379, 2609, 10082, 8191, 2364,
     45027735},
    {"darwin-arm64", "macho", "macos", "arm64",
     "50fbf594d6526c9aae7d66b8b45a08d89ae6b863e2bc9777bfb277d6521a7458",
     "c9b5341637becbd423ddffc5b254afb645682a3868cb708bbc6cc0e7bb419937",
     105100333, 240664432, 68206600, 171766883, 2609, 10082, 8191, 2364,
     45035899},
    {"win32-x64", "pe", "windows", "x64",
     "f04bb1c18698dc6562912a573b031306225f5e202df4fe6bd2317351f5e492c5",
     "76de74f7694a5bdc03378a5f837c9e132827bf004c98c276bc4e9094ff06769a",
     117726124, 260489376, 93839368, 166639183, 2586, 9998, 7167, 2343,
     44717469},
    {"win32-arm64", "pe", "windows", "arm64",
     "7019eb71ba03dc70e76e7e909d5132192192948abe02bb18105ca34cae976547",
     "a652a17f5a324d2d9ad9995cc047145f8176b9a4b4f28bbd3aa0708a22679e3b",
     113601474, 248013984, 81410056, 166593307, 2586, 9998, 8191, 2343,
     44709481},
    {"linux-x64-musl", "elf", "linux", "x64",
     "88fcfa54fdb0ea24594111378908d6488fb975da8b139f5fb5e38d8256ffba5b",
     "94a1ccc6ffc9b0889c6b2b80e202885c8a575766775377398daa4db5fd89417f",
     112639151, 250818648, 82862088, 167870511, 2589, 10006, 8191, 2345,
     44760179},
    {"linux-arm64-musl", "elf", "linux", "arm64",
     "7895ef5b714e675152a18058e83cbf9d5f01d758dee97c2e40969f71f2c884f3",
     "b0bbc936a82930933d7d5645081050d5f827a946cf6abc6575383b777a0feec0",
     112659894, 248788800, 80936968, 167742821, 2589, 10006, 8191, 2345,
     44760184}};
class WebClaudeDistribution : public ::testing::TestWithParam<Distribution> {};

TEST_P(WebClaudeDistribution, CapturedArchiveGraphAndSourcesWhenSupplied) {
  const char *Directory = std::getenv("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
  if (!Directory)
    GTEST_SKIP() << "Optional captured fixed-version platform packages absent";
  if (!packageArchiveAvailable() || !packageGzipAvailable())
    GTEST_SKIP() << "Native gzip and the archive path policy are required";
  const auto &T = GetParam();
  const auto S =
      capture((std::filesystem::path(Directory) / T.Target / "input").string(),
              Limits{});
  ASSERT_EQ(S.Artifacts.size(), 4);
  const auto &Original = S.Artifacts[1];
  ASSERT_EQ(Original.Content.size(), T.ArchiveBytes);
  ASSERT_EQ(Original.BlobHash, T.ArchiveHash);
  const auto Integrity = verifyPackageIntegrity(
      Original, registryPackageIntegrity(S.Artifacts[2]));
  ASSERT_EQ(Integrity.Status, "match");
  EXPECT_EQ(Integrity.Algorithm, "sha512");
  const auto A = extractPackageArchive(Original, "tgz");
  ASSERT_EQ(A.Members.size(), 4);
  const auto &M = A.Members.front();
  ASSERT_TRUE(M.available());
  ASSERT_EQ(M.Size, T.ExecutableBytes);
  ASSERT_EQ(M.BlobHash, T.ExecutableHash);
  EXPECT_EQ(M.Offset, 512);
  const auto Namespace = packageArchiveNamespace(A);
  const auto Selected =
      std::find_if(Namespace.Artifacts.begin(), Namespace.Artifacts.end(),
                   [&](const Artifact &I) { return I.ID == M.ID; });
  ASSERT_NE(Selected, Namespace.Artifacts.end());
  const auto E = extractBun(*Selected);
  const auto Profile = std::string("bun-71d0d439-prelinked-") + T.Platform +
                       "-" + T.Architecture + "-" + T.Format + "-v1";
  ASSERT_EQ(E.Profile, Profile);
  EXPECT_EQ(E.ContainerFormat, T.Format);
  EXPECT_EQ(E.Platform, T.Platform);
  EXPECT_EQ(E.Architecture, T.Architecture);
  EXPECT_EQ(E.GraphOffset, T.GraphOffset);
  EXPECT_EQ(E.GraphSize, T.GraphBytes);
  EXPECT_EQ(E.Modules.size(), T.Modules);
  EXPECT_EQ(E.Regions.size(), T.Regions);
  EXPECT_EQ(E.Flags, T.Flags);
  EXPECT_EQ(E.EntryPoint, 6);
  EXPECT_EQ(E.StartupCount, 7);
  EXPECT_EQ(E.PrelinkedModuleFiles.size(), T.Sources);
  for (const auto Index : E.PrelinkedModuleFiles) {
    ASSERT_LT(Index, E.Modules.size());
    EXPECT_FALSE(E.Modules[Index].SourceArtifactID.empty());
  }
  EXPECT_TRUE(E.RuntimeOptions);
  ASSERT_LT(E.CompileArguments, E.Regions.size());
  EXPECT_EQ(E.Regions[E.CompileArguments].Kind, "compile_exec_argv");
  EXPECT_EQ(E.Regions[E.CompileArguments].Content.size(), 0);
  uint64_t Sources = 0, Bytes = 0, Maps = 0;
  for (const auto &Module : E.Modules) {
    Maps += Module.SourceMap != NoBunIndex;
    if (Module.SourceArtifactID.empty())
      continue;
    const auto Text = bunSourceBytes(E, Module, MaxBunDecodedSourceBytes);
    EXPECT_TRUE(validUtf8(Text));
    ++Sources;
    Bytes += Text.size();
  }
  EXPECT_EQ(Sources, T.Sources);
  EXPECT_EQ(Bytes, T.SourceBytes);
  EXPECT_EQ(Maps, 0);
  RecordProperty("decoded_sources", std::to_string(Sources));
  RecordProperty("decoded_source_bytes", std::to_string(Bytes));
}

INSTANTIATE_TEST_SUITE_P(NpmPlatformInputs, WebClaudeDistribution,
                         ::testing::ValuesIn(Cases),
                         [](const ::testing::TestParamInfo<Distribution> &I) {
                           std::string Name(I.param.Target);
                           std::replace(Name.begin(), Name.end(), '-', '_');
                           return Name;
                         });
} // namespace
