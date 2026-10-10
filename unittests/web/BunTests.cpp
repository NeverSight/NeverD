#include "BunFixture.h"
#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"

#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::test;
class WebBun : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-bun", Directory));
    Root = Directory.str().str();
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  Artifact input(std::string_view Bytes) {
    const auto Path = Root / "input";
    {
      std::ofstream File(Path, std::ios::binary);
      File.write(Bytes.data(), Bytes.size());
      if (!File.good())
        throw std::runtime_error("fixture write");
    }
    return capture(Path.string(), Limits{}).Artifacts.at(0);
  }
  void refuse(const std::string &Bytes, const char *Code) {
    try {
      (void)extractBun(input(Bytes));
      FAIL() << "accepted " << Code;
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), Code);
    }
  }
};

std::string fixture(std::string_view Name) {
  std::ifstream F(std::string(NEVERD_WEB_FIXTURE_DIR) + "/bun/" +
                      std::string(Name),
                  std::ios::binary);
  if (!F)
    throw std::runtime_error("missing corpus fixture");
  return {std::istreambuf_iterator<char>(F), {}};
}
llvm::json::Value manifest(std::string_view Name) {
  auto Parsed =
      llvm::json::parse(fixture(std::string(Name) + ".manifest.json"));
  if (!Parsed)
    throw std::runtime_error("invalid corpus manifest");
  return std::move(*Parsed);
}
std::string field(const llvm::json::Value &V, const char *Key) {
  return V.getAsObject()->getString(Key)->str();
}
void checkGolden(const BunExtraction &E, const llvm::json::Value &Expected) {
  const auto &Modules = *Expected.getAsObject()->getArray("modules");
  ASSERT_EQ(E.Modules.size(), Modules.size());
  for (size_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    const auto &R = *Modules[I].getAsObject()->getArray("ranges");
    const std::array<uint32_t, 6> Actual{M.Name,       M.Contents,
                                         M.SourceMap,  M.Bytecode,
                                         M.ModuleInfo, M.BytecodeOrigin};
    for (size_t P = 0; P < Actual.size(); ++P) {
      const auto Size = std::stoull(field(R[P], "size"));
      if (P > 1 && Size == 0) {
        EXPECT_EQ(Actual[P], NoBunIndex);
        continue;
      }
      ASSERT_LT(Actual[P], E.Regions.size());
      const auto &V = E.Regions[Actual[P]];
      EXPECT_EQ(V.Offset - E.GraphOffset, std::stoull(field(R[P], "offset")));
      EXPECT_EQ(V.Content.size(), Size);
      EXPECT_EQ(V.BlobHash, field(R[P], "sha256"));
    }
    EXPECT_EQ(M.Encoding, Modules[I].getAsObject()->getInteger("encoding"));
    EXPECT_EQ(M.Format, Modules[I].getAsObject()->getInteger("format"));
    EXPECT_EQ(M.Loader, Modules[I].getAsObject()->getInteger("loader"));
  }
}

#ifndef _WIN32
TEST_F(WebBun, AllQualifiedContainersRetainGraphAndMemberEvidence) {
  for (const bool ARM64 : {false, true}) {
    for (const bool Prelinked : {false, true}) {
      const BunFixture F(true, "map", "asset", Prelinked);
      for (const auto *Format : {"elf", "macho", "pe"}) {
        SCOPED_TRACE(std::string(Format) + (ARM64 ? " arm64" : " x64"));
        const std::string Platform = std::string_view(Format) == "elf" ? "linux"
                                     : std::string_view(Format) == "macho"
                                         ? "macos"
                                         : "windows";
        const auto Graph =
            Platform == "windows" ? windowsGraph(F.Graph) : F.Graph;
        const auto Bytes = Platform == "linux" ? bunELF(Graph, ARM64 ? 183 : 62)
                           : Platform == "macos" ? bunMachO(Graph, ARM64)
                                                 : bunPE(Graph, ARM64);
        const auto A = input(Bytes);
        const auto E = extractBun(A);
        EXPECT_EQ(E.ID, extractBun(A).ID);
        EXPECT_EQ(E.ContainerFormat, Format);
        EXPECT_EQ(E.Platform, Platform);
        EXPECT_EQ(E.Architecture, ARM64 ? "arm64" : "x64");
        EXPECT_EQ(
            E.Profile,
            std::string(Prelinked ? "bun-71d0d439-prelinked-" : "bun-1.4.2-") +
                Platform + (ARM64 ? "-arm64-" : "-x64-") + Format + "-v1");
        ASSERT_EQ(E.Modules.size(), 3);
        EXPECT_EQ(bunSourceBytes(E, E.Modules[1], 1024),
                  "export const x = '中文🌱';");
        EXPECT_EQ(Bytes.substr(E.GraphOffset, E.GraphSize), Graph);
        for (const auto &R : E.Regions) {
          EXPECT_EQ(R.BlobHash, sha256(std::string_view(Bytes).substr(
                                    R.Offset, R.Content.size())));
          EXPECT_EQ(R.Content.read(0, R.Content.size()),
                    Bytes.substr(R.Offset, R.Content.size()));
        }
      }
    }
  }
}

TEST_F(WebBun, MachORejectsMalformedCommandsMappingsAndPadding) {
  const BunFixture F;
  const auto Original = bunMachO(F.Graph, true);
  auto Mutate = [&](uint64_t At, uint64_t Value, unsigned Width,
                    const char *Code) {
    auto Bytes = Original;
    put(Bytes, At, Value, Width);
    refuse(Bytes, Code);
  };
  Mutate(0, 0xbebafeca, 4, "bun_unsupported_container");
  Mutate(4, 12, 4, "bun_unsupported_container");
  Mutate(8, 2, 4, "bun_unsupported_container");
  Mutate(16, 4097, 4, "bun_unsupported_macho_commands");
  Mutate(36, 0, 4, "bun_invalid_macho_command");
  Mutate(36, 156, 4, "bun_invalid_macho_command");
  Mutate(36, 160, 4, "bun_invalid_macho_sections");
  Mutate(96, 4097, 4, "bun_invalid_macho_sections");
  Mutate(92, 7, 4, "bun_invalid_macho_segment");
  Mutate(72, UINT64_MAX, 8, "bun_range_out_of_bounds");
  Mutate(136, 0, 8, "bun_invalid_macho_section");
  Mutate(152, 16385, 4, "bun_ambiguous_load_mapping");
  Mutate(168, 0, 4, "bun_invalid_macho_section");
  Mutate(192, 2, 4, "bun_unsupported_macho_platform");
  Mutate(204, UINT32_MAX, 4, "bun_invalid_macho_command");
  Mutate(144, F.Graph.size() + 9, 8, "bun_invalid_graph_header");
  Mutate(16384, F.Graph.size() + 1, 8, "bun_invalid_graph_header");
  Mutate(Original.size() - 1, 1, 1, "bun_invalid_section_padding");
  auto Alias = Original;
  Alias.replace(184, 152, Original.substr(32, 152));
  put(Alias, 16, 2, 4);
  put(Alias, 20, 304, 4);
  refuse(Alias, "bun_duplicate_section");
  Alias[192] = 'X';
  Alias[256] = 'X';
  refuse(Alias, "bun_ambiguous_load_mapping");
  for (const auto N : {64U, 151U, 16388U})
    EXPECT_THROW(extractBun(input(Original.substr(0, N))), Error);
}

TEST_F(WebBun, MachOTypedCommandsCannotAliasGraphOrMisidentifyPlatform) {
  const BunFixture F;
  const auto Original = bunMachO(F.Graph, true);
  auto Append = [&](uint64_t Command, uint64_t Size) {
    auto B = Original;
    put(B, 16, 3, 4);
    put(B, 20, 176 + Size, 4);
    put(B, 208, Command, 4);
    put(B, 212, Size, 4);
    return B;
  };
  for (auto Command :
       {0x2U, 0xbU, 0x22U, 0x1dU, 0x26U, 0x29U, 0x80000033U, 0x80000034U,
        0x80000028U, 0x31U, 0x32U, 0x24U, 0x1bU, 0xeU, 0xcU})
    refuse(Append(Command, 8), "bun_invalid_macho_command");
  refuse(Append(0xdeadbeef, 8), "bun_unsupported_macho_command");
  for (auto Command : {0x1dU, 0x26U, 0x29U, 0x80000033U, 0x80000034U}) {
    auto B = Append(Command, 16);
    put(B, 216, 16384, 4);
    put(B, 220, 8, 4);
    refuse(B, "bun_ambiguous_load_mapping");
    put(B, 216, B.size(), 4);
    refuse(B, "bun_range_out_of_bounds");
  }
  auto Symbols = Append(0x2, 24);
  put(Symbols, 216, 16384, 4);
  put(Symbols, 220, 1, 4);
  refuse(Symbols, "bun_ambiguous_load_mapping");
  auto Entry = Append(0x80000028, 24);
  put(Entry, 216, 16384, 8);
  refuse(Entry, "bun_ambiguous_load_mapping");
  auto Duplicate = Append(0x32, 24);
  put(Duplicate, 216, 1, 4);
  refuse(Duplicate, "bun_unsupported_macho_platform");
  refuse(Append(0x24, 16), "bun_unsupported_macho_platform");
  auto Missing = Original;
  put(Missing, 16, 1, 4);
  put(Missing, 20, 152, 4);
  refuse(Missing, "bun_unsupported_macho_platform");
  auto Legacy = Original;
  put(Legacy, 20, 168, 4);
  put(Legacy, 184, 0x24, 4);
  put(Legacy, 188, 16, 4);
  EXPECT_EQ(extractBun(input(Legacy)).Platform, "macos");
  for (auto Command : {0x25U, 0x2fU, 0x30U}) {
    put(Legacy, 184, Command, 4);
    refuse(Legacy, "bun_unsupported_macho_platform");
  }
  auto Dylib = Append(0xc, 32);
  put(Dylib, 216, 24, 4);
  Dylib.replace(232, 8, "bad-path");
  refuse(Dylib, "bun_invalid_macho_command");
  Dylib[239] = '\0';
  EXPECT_EQ(extractBun(input(Dylib)).Platform, "macos");
}

TEST_F(WebBun, PERejectsMalformedHeadersAliasesDirectoriesAndPadding) {
  const BunFixture F;
  const auto Original = bunPE(windowsGraph(F.Graph), false);
  auto Mutate = [&](uint64_t At, uint64_t Value, unsigned Width,
                    const char *Code) {
    auto Bytes = Original;
    put(Bytes, At, Value, Width);
    refuse(Bytes, Code);
  };
  Mutate(60, UINT32_MAX, 4, "bun_range_out_of_bounds");
  Mutate(132, 0x14c, 2, "bun_unsupported_container");
  Mutate(134, 97, 2, "bun_unsupported_pe_sections");
  Mutate(152, 0x10b, 2, "bun_unsupported_container");
  Mutate(188, 3, 4, "bun_invalid_pe_headers");
  Mutate(212, 256, 4, "bun_invalid_pe_headers");
  Mutate(260, 17, 4, "bun_invalid_pe_headers");
  Mutate(404, 0, 4, "bun_invalid_pe_section");
  Mutate(404, 0xfffff000, 4, "bun_range_out_of_bounds");
  Mutate(412, 0, 4, "bun_invalid_pe_section");
  Mutate(428, 0xe0000040, 4, "bun_invalid_pe_section");
  Mutate(512, F.Graph.size() + 1, 8, "bun_invalid_graph_header");
  Mutate(Original.size() - 1, 1, 1, "bun_invalid_section_padding");
  auto Alias = Original;
  put(Alias, 134, 2, 2);
  Alias.replace(432, 40, Original.substr(392, 40));
  refuse(Alias, "bun_duplicate_section");
  Alias[432] = 'X';
  refuse(Alias, "bun_ambiguous_load_mapping");
  for (const auto I : {0U, 4U}) {
    Alias = Original;
    put(Alias, 264 + I * 8, I == 4 ? 512 : 4096, 4);
    put(Alias, 268 + I * 8, 8, 4);
    refuse(Alias, "bun_ambiguous_load_mapping");
  }
  refuse(bunPE(F.Graph, false), "bun_invalid_module_name");
  for (const auto N : {64U, 239U, 516U})
    EXPECT_THROW(extractBun(input(Original.substr(0, N))), Error);
}

TEST_F(WebBun, SourceDecoderStreamsLargeStorageAndCrossChunkSurrogates) {
  BunExtraction E;
  BunModule M;
  M.SourceArtifactID = "source";
  M.Contents = 0;
  M.Encoding = 1;
  std::string Large(5 * 1024 * 1024, 'a');
  Large.back() = char(0xe9);
  BunRegion R;
  R.Content = input(Large).Content;
  E.Regions.push_back(R);
  const auto Text = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
  EXPECT_EQ(Text.size(), Large.size() + 1);
  EXPECT_TRUE(Text.ends_with("é"));
  EXPECT_THROW(bunSourceBytes(E, M, 1024 * 1024), Error);
  std::string UTF16;
  for (unsigned I = 0; I < 32767; ++I)
    UTF16.append("a\0", 2);
  UTF16.append("\x3d\xd8\x00\xde", 4);
  E.Regions[0].Content = input(UTF16).Content;
  M.Encoding = 2;
  const auto Unicode = bunSourceBytes(E, M, 1024 * 1024);
  EXPECT_EQ(Unicode, std::string(32767, 'a') + "😀");
  const auto Anchor = bunSourceRange(E, M, 32767, 4, 1024 * 1024);
  EXPECT_EQ(Anchor.Offset, 65534);
  EXPECT_EQ(Anchor.Size, 4);
  UTF16.pop_back();
  E.Regions[0].Content = input(UTF16).Content;
  EXPECT_THROW(bunSourceBytes(E, M, 1024 * 1024), Error);
}

TEST_F(WebBun, PrelinkedGraphAndRuntimeMetadataHaveTheirOwnLayoutProfile) {
  const BunFixture F(true, "map", "asset", true);
  const auto E = extractBun(input(F.Bytes));
  EXPECT_EQ(E.Profile, BunPrelinkedProfile);
  EXPECT_EQ(E.Modules.size(), 3);
  EXPECT_EQ(bunSourceBytes(E, E.Modules[0], 1024),
            "export const secret = 'BUN_SOURCE_CANARY';");
  const auto R =
      std::find_if(E.Regions.begin(), E.Regions.end(), [](const auto &V) {
        return V.Kind == "prelinked_module_graph";
      });
  ASSERT_NE(R, E.Regions.end());
  EXPECT_EQ(R->Content.read(0, R->Content.size()), "PRELINKED_CANARY");
  EXPECT_EQ(extractBun(input(BunFixture{}.Bytes)).Profile, BunProfile);
}

TEST_F(WebBun, PrelinkedIndicesPointersAndUnknownCacheAliasingFailClosed) {
  const BunFixture F(true, "map", "asset", true);
  auto Mutate = [&](uint64_t At, uint64_t Value, const char *Code) {
    auto B = F.Bytes;
    put(B, 4104 + At, Value, 4);
    refuse(B, Code);
  };
  Mutate(F.PrelinkedRecord + 8, 4, "bun_invalid_prelinked_files");
  Mutate(F.PrelinkedRecord + 12, 3, "bun_invalid_prelinked_files");
  Mutate(F.PrelinkedRecord + 16, 0, "bun_invalid_prelinked_files");
  Mutate(F.PrelinkedRecord + 4, 0, "bun_empty_shared_table");
  Mutate(F.PrelinkedRecord, get(F.Graph, F.PrelinkedRecord, 4) + 1,
         "bun_invalid_bytecode_alignment");
  Mutate(F.Footer + 28, 0x3bf0, "bun_unsupported_graph_flags");
}

TEST_F(WebBun, PreservedCompilerGraphsMatchIndependentGoldenMemberRanges) {
  for (const auto Name :
       {"plain", "unicode", "utf16", "asset-map", "cache-map"}) {
    SCOPED_TRACE(Name);
    const auto Expected = manifest(Name);
    const auto Graph = fixture(std::string(Name) + ".graph.bin");
    ASSERT_EQ(sha256(Graph), field(Expected, "graph_sha256"));
    // Only the graph bytes are original compiler output. This deliberately
    // synthetic native wrapper cannot establish producer/version provenance.
    const auto E = extractBun(input(bunELF(Graph)));
    checkGolden(E, Expected);
    for (const auto &M : E.Modules)
      if (!M.SourceArtifactID.empty())
        EXPECT_FALSE(bunSourceBytes(E, M, 1024 * 1024).empty());
    if (std::string_view(Name) == "utf16") {
      ASSERT_EQ(E.Modules[0].Encoding, 2);
      EXPECT_NE(bunSourceBytes(E, E.Modules[0], 1024).find("中文 🌱"),
                std::string::npos);
    }
  }
}

TEST_F(WebBun, FullCompilerContainersMatchRecordedGoldenHashesWhenSupplied) {
  const auto *Directory = std::getenv("NEVERD_BUN_142_CORPUS");
  if (!Directory)
    GTEST_SKIP() << "Full pinned compiler corpus not supplied; preserved graph "
                    "checks remain independent";
  for (const auto Name :
       {"plain", "unicode", "utf16", "asset-map", "cache-map"}) {
    SCOPED_TRACE(Name);
    const auto Expected = manifest(Name);
    const auto Path =
        std::filesystem::path(Directory) / (std::string(Name) + ".elf");
    const auto Snapshot = capture(Path.string(), Limits{});
    ASSERT_EQ(Snapshot.Artifacts.size(), 1);
    const auto &A = Snapshot.Artifacts[0];
    ASSERT_EQ(A.BlobHash, field(Expected, "full_elf_sha256"));
    const auto E = extractBun(A);
    EXPECT_EQ(E.GraphOffset, std::stoull(field(Expected, "graph_offset")));
    checkGolden(E, Expected);
  }
}

struct CrossTarget {
  const char *Name, *Format, *Platform, *Architecture;
};
constexpr CrossTarget CrossTargets[] = {
    {"linux-arm64", "elf", "linux", "arm64"},
    {"macos-x64", "macho", "macos", "x64"},
    {"macos-arm64", "macho", "macos", "arm64"},
    {"windows-x64", "pe", "windows", "x64"},
    {"windows-arm64", "pe", "windows", "arm64"}};

TEST_F(WebBun, CrossPlatformCompilerGraphsMatchIndependentGoldenMemberRanges) {
  for (const auto &T : CrossTargets)
    for (const char *Variant : {"plain", "utf16", "asset-map", "cache-map"}) {
      const auto Name = std::string("cross/") + T.Name + "-" + Variant;
      SCOPED_TRACE(Name);
      const auto Expected = manifest(Name);
      const auto Graph = fixture(Name + ".graph.bin");
      ASSERT_EQ(sha256(Graph), field(Expected, "graph_sha256"));
      const bool ARM64 = std::string_view(T.Architecture) == "arm64";
      const auto Bytes =
          std::string_view(T.Format) == "elf"     ? bunELF(Graph, 183)
          : std::string_view(T.Format) == "macho" ? bunMachO(Graph, ARM64)
                                                  : bunPE(Graph, ARM64);
      const auto E = extractBun(input(Bytes));
      checkGolden(E, Expected);
      EXPECT_EQ(E.Platform, T.Platform);
      EXPECT_EQ(E.Architecture, T.Architecture);
      for (const auto &M : E.Modules)
        if (!M.SourceArtifactID.empty())
          EXPECT_FALSE(bunSourceBytes(E, M, 1024 * 1024).empty());
      if (std::string_view(Variant) == "utf16") {
        ASSERT_EQ(E.Modules[0].Encoding, 2);
        EXPECT_NE(bunSourceBytes(E, E.Modules[0], 1024).find("中文 🌱"),
                  std::string::npos);
      }
    }
}

TEST_F(WebBun, CrossPlatformFullCompilerContainersWhenSupplied) {
  const auto *Directory = std::getenv("NEVERD_BUN_142_CROSS_CORPUS");
  if (!Directory)
    GTEST_SKIP() << "Full cross-platform Bun compiler corpus not supplied";
  for (const auto &T : CrossTargets)
    for (const char *Variant : {"plain", "utf16", "asset-map", "cache-map"}) {
      const auto Name = std::string(T.Name) + "-" + Variant;
      SCOPED_TRACE(Name);
      const auto Expected = manifest("cross/" + Name);
      const auto Path =
          std::filesystem::path(Directory) /
          (Name + (std::string_view(T.Format) == "pe" ? ".exe" : ""));
      const auto Snapshot = capture(Path.string(), Limits{});
      ASSERT_EQ(Snapshot.Artifacts.size(), 1);
      const auto &A = Snapshot.Artifacts.front();
      ASSERT_EQ(A.BlobHash, field(Expected, "full_container_sha256"));
      ASSERT_EQ(A.Content.size(),
                std::stoull(field(Expected, "full_container_size")));
      const auto E = extractBun(A);
      EXPECT_EQ(E.GraphOffset, std::stoull(field(Expected, "graph_offset")));
      EXPECT_EQ(E.GraphSize, std::stoull(field(Expected, "graph_size")));
      EXPECT_EQ(E.ContainerFormat, T.Format);
      EXPECT_EQ(E.Platform, T.Platform);
      EXPECT_EQ(E.Architecture, T.Architecture);
      checkGolden(E, Expected);
      if (std::string_view(T.Format) == "macho" &&
          std::string_view(Variant) == "plain") {
        std::string Bytes;
        Bytes.reserve(A.Content.size());
        for (uint64_t At = 0; At < A.Content.size();) {
          const auto Size =
              std::min<uint64_t>(MaxBlobReadBytes, A.Content.size() - At);
          Bytes += A.Content.read(At, Size);
          At += Size;
        }
        uint64_t At = 32;
        unsigned Checked = 0;
        for (uint64_t I = 0; I < get(Bytes, 16, 4); ++I) {
          const auto Command = get(Bytes, At, 4);
          if (Command == 0x32) {
            auto OtherPlatform = Bytes;
            put(OtherPlatform, At + 8, 2, 4);
            refuse(OtherPlatform, "bun_unsupported_macho_platform");
            ++Checked;
          } else if (Command == 0x1d || Command == 0x80000034) {
            auto Alias = Bytes;
            put(Alias, At + 8, E.GraphOffset, 4);
            put(Alias, At + 12, 1, 4);
            refuse(Alias, "bun_ambiguous_load_mapping");
            ++Checked;
          }
          At += get(Bytes, At + 4, 4);
        }
        EXPECT_EQ(Checked, 3);
      }
    }
}

TEST_F(WebBun, ClaudeCode21296FullContainerWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional official Claude Code 2.1.296 linux-x64 artifact "
                    "not supplied; no target code is redistributed";
  const auto Snapshot = capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1);
  const auto &Original = Snapshot.Artifacts.front();
  ASSERT_EQ(Original.Content.size(), 257068216);
  // Official HTTPS release manifest, captured 2026-10-10. This is an exact
  // artifact qualification, not verification of its publisher signature.
  ASSERT_EQ(Original.BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Original);
  ASSERT_EQ(E.Profile, BunPrelinkedProfile);
  ASSERT_EQ(E.Modules.size(), 2589);
  EXPECT_EQ(E.Regions.size(), 10005);
  EXPECT_EQ(E.EntryPoint, 6);
  EXPECT_EQ(E.StartupCount, 7);
  EXPECT_EQ(E.GraphOffset, 89120776);
  uint64_t Sources = 0, Bytes = 0, Maps = 0, Caches = 0;
  for (const auto &M : E.Modules) {
    Maps += M.SourceMap != NoBunIndex;
    Caches += M.Bytecode != NoBunIndex;
    if (!M.SourceArtifactID.empty()) {
      ++Sources;
      const auto Source = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
      EXPECT_TRUE(validUtf8(Source));
      Bytes += Source.size();
    }
  }
  EXPECT_EQ(Sources, 2345);
  EXPECT_EQ(Bytes, 44768763);
  EXPECT_EQ(Maps, 0);
  EXPECT_EQ(Caches, 2343);
}

TEST_F(WebBun, SourceAssetsMapsAndCachesRetainOriginalRangesAndHashes) {
  const BunFixture F;
  const auto Original = input(F.Bytes);
  const auto E = extractBun(Original), Again = extractBun(Original);
  ASSERT_EQ(E.Modules.size(), 3);
  EXPECT_EQ(E.ID, Again.ID);
  EXPECT_EQ(E.GraphOffset, 4104);
  EXPECT_EQ(E.StartupCount, 2);
  EXPECT_EQ(E.EntryPoint, 0);
  for (const auto &R : E.Regions) {
    EXPECT_EQ(R.BlobHash, sha256(std::string_view(F.Bytes).substr(
                              R.Offset, R.Content.size())));
    EXPECT_EQ(R.Content.read(0, R.Content.size()),
              F.Bytes.substr(R.Offset, R.Content.size()));
  }
  const auto &M = E.Modules[0];
  EXPECT_NE(M.ID, E.Regions[M.Contents].ID);
  EXPECT_NE(M.SourceArtifactID, E.Regions[M.Contents].ID);
  EXPECT_EQ(bunSourceBytes(E, M, 1024),
            "export const secret = 'BUN_SOURCE_CANARY';");
  EXPECT_EQ(bunSourceBytes(E, E.Modules[1], 1024),
            "export const x = '中文🌱';");
  EXPECT_TRUE(E.Modules[2].SourceArtifactID.empty());
  EXPECT_NE(M.Bytecode, NoBunIndex);
  EXPECT_NE(M.SourceMap, NoBunIndex);
  EXPECT_NE(M.ModuleInfo, NoBunIndex);
  EXPECT_NE(M.BytecodeOrigin, NoBunIndex);
  std::filesystem::remove(Root / "input");
  EXPECT_EQ(bunSourceBytes(E, M, 1024),
            "export const secret = 'BUN_SOURCE_CANARY';");
}

TEST_F(WebBun, MalformedFooterAndUnknownLayoutDoNotProducePartialExtraction) {
  const BunFixture F;
  const auto At = 4104 + F.Footer;
  auto Mutate = [&](uint64_t Offset, uint64_t Value, unsigned Width,
                    const char *Code) {
    auto B = F.Bytes;
    put(B, Offset, Value, Width);
    refuse(B, Code);
  };
  Mutate(4096, F.Graph.size() + 1, 8, "bun_invalid_graph_header");
  Mutate(At, F.Footer - 1, 8, "bun_invalid_byte_count");
  Mutate(At + 32, 0, 1, "bun_invalid_trailer");
  Mutate(At + 28, 0x800003f0, 4, "bun_unsupported_graph_flags");
  Mutate(At + 28, 0x3e0, 4, "bun_unsupported_graph_flags");
  Mutate(At + 12, 157, 4, "bun_invalid_module_table");
  Mutate(At + 16, 3, 4, "bun_invalid_entry_point");
  Mutate(At + 20, 0xffffffff, 4, "bun_range_out_of_bounds");
  Mutate(At + 8, 0xffffffff, 4, "bun_range_out_of_bounds");
  Mutate(At + 24, 0xffffffff, 4, "bun_range_out_of_bounds");
}

TEST_F(WebBun, ELFMappingAndContainerBoundsAreRequired) {
  const BunFixture F;
  auto Mutate = [&](uint64_t Offset, uint64_t Value, unsigned Width,
                    const char *Code) {
    auto B = F.Bytes;
    put(B, Offset, Value, Width);
    refuse(B, Code);
  };
  Mutate(4, 1, 1, "bun_unsupported_container");
  Mutate(18, 40, 2, "bun_unsupported_container");
  Mutate(40, UINT64_MAX, 8, "bun_range_out_of_bounds");
  Mutate(60, 0xffff, 2, "bun_unsupported_elf_tables");
  Mutate(68, 5, 4, "bun_ambiguous_load_mapping");
  Mutate(80, 0x800000, 8, "bun_ambiguous_load_mapping");
  Mutate(96, UINT64_MAX, 8, "bun_range_out_of_bounds");
  Mutate(104, UINT64_MAX, 8, "bun_range_out_of_bounds");
  Mutate(112, 3, 8, "bun_invalid_load_segment");
  const auto SH = get(F.Bytes, 40, 8);
  Mutate(SH + 128 + 24, 128, 8, "bun_overlapping_section");
  for (const auto N : {0U, 63U, 119U, 4100U})
    EXPECT_THROW(extractBun(input(F.Bytes.substr(0, N))), Error);
}

TEST_F(WebBun, PointerAliasingTerminatorsEnumsAndNamesFailClosed) {
  const BunFixture F;
  const auto At = 4104 + F.Table;
  auto Mutate = [&](uint64_t Offset, uint64_t Value, unsigned Width,
                    const char *Code) {
    auto B = F.Bytes;
    put(B, Offset, Value, Width);
    refuse(B, Code);
  };
  Mutate(At + 48, 3, 1, "bun_unsupported_module_encoding");
  Mutate(At + 49, 255, 1, "bun_unsupported_module_encoding");
  Mutate(At + 50, 0, 1, "bun_unsupported_module_encoding");
  Mutate(At + 51, 2, 1, "bun_unsupported_module_encoding");
  Mutate(At + 24, 121, 4, "bun_invalid_bytecode_alignment");
  Mutate(At + 12, UINT32_MAX, 4, "bun_range_out_of_bounds");
  const auto Name = get(F.Bytes, At, 4), NameSize = get(F.Bytes, At + 4, 4);
  Mutate(4104 + Name + NameSize, 1, 1, "bun_missing_terminator");
  Mutate(At + 4, 32769, 4, "bun_name_budget_exceeded");
  auto Duplicate = F.Bytes;
  put(Duplicate, At + 52, Name, 4);
  put(Duplicate, At + 56, NameSize, 4);
  refuse(Duplicate, "bun_invalid_module_name");
  auto Alias = F.Bytes;
  put(Alias, At + 32, get(Alias, At + 24, 4), 4);
  refuse(Alias, "bun_overlapping_payloads");
}

TEST_F(WebBun, TextProjectionBudgetsAndUnpairedSurrogatesKeepRawEvidence) {
  const BunFixture F(false);
  const auto E = extractBun(input(F.Bytes));
  EXPECT_THROW(bunSourceBytes(E, E.Modules[0], 1), Error);
  const auto Good = bunSourceBytes(E, E.Modules[1], 1024);
  EXPECT_EQ(bunSourceBytes(E, E.Modules[1], Good.size()), Good);
  EXPECT_THROW(bunSourceBytes(E, E.Modules[1], Good.size() - 1), Error);
  auto Bad = F.Bytes;
  const auto Off = E.Regions[E.Modules[1].Contents].Offset;
  put(Bad, Off, 0xd800, 2);
  const auto Raw = extractBun(input(Bad));
  EXPECT_THROW(bunSourceBytes(Raw, Raw.Modules[1], 1024), Error);
  EXPECT_EQ(Raw.Regions[Raw.Modules[1].Contents].Content.read(0, 2),
            std::string("\0\xd8", 2));
  EXPECT_THROW(bunSourceBytes(E, E.Modules[2], 1024), Error);
}

TEST_F(WebBun, SourceRangesFollowUtf16ScalarsAndExcludeStorageTerminators) {
  const BunFixture F;
  const auto E = extractBun(input(F.Bytes));
  const auto &M = E.Modules[1];
  const auto &R = E.Regions[M.Contents];
  const std::string Text = "export const x = '中文🌱';";
  const auto CJK = Text.find("中"), Plant = Text.find("🌱");
  const auto First = bunSourceRange(E, M, CJK, 3, Text.size());
  EXPECT_EQ(First.Text, Text);
  EXPECT_EQ(First.Offset, R.Offset + 2 * CJK);
  EXPECT_EQ(First.Size, 2);
  EXPECT_EQ(F.Bytes.substr(First.Offset, First.Size),
            std::string("\x2d\x4e", 2));
  const auto Pair = bunSourceRange(E, M, Plant, 4, Text.size());
  EXPECT_EQ(Pair.Offset, R.Offset + 2 * CJK + 4);
  EXPECT_EQ(Pair.Size, 4);
  EXPECT_EQ(F.Bytes.substr(Pair.Offset, Pair.Size),
            std::string("\x3c\xd8\x31\xdf", 4));
  const auto Whole = bunSourceRange(E, M, 0, Text.size(), Text.size());
  EXPECT_EQ(Whole.Offset, R.Offset);
  EXPECT_EQ(Whole.Size, R.Content.size());
  const auto EOFRange = bunSourceRange(E, M, Text.size(), 0, Text.size());
  EXPECT_EQ(EOFRange.Offset, R.Offset + R.Content.size());
  EXPECT_EQ(EOFRange.Size, 0);
  EXPECT_THROW(bunSourceRange(E, M, CJK + 1, 0, Text.size()), Error);
  EXPECT_THROW(bunSourceRange(E, M, Plant, 2, Text.size()), Error);
  EXPECT_THROW(bunSourceRange(E, M, 0, Text.size() + 1, Text.size() + 1),
               Error);
  EXPECT_THROW(bunSourceRange(E, M, UINT64_MAX, 1, Text.size()), Error);
  EXPECT_THROW(bunSourceRange(E, M, 1, UINT64_MAX, Text.size()), Error);
  EXPECT_THROW(bunSourceRange(E, M, 0, 0, Text.size() - 1), Error);
  EXPECT_THROW(bunSourceRange(E, E.Modules[2], 0, 0, 1024), Error);
}

TEST_F(WebBun, Latin1AndClientUtf8HaveDifferentStorageCoordinateRules) {
  const BunFixture F;
  const auto E = extractBun(input(F.Bytes));
  const auto Base = E.Regions[E.Modules[0].Contents].Offset;
  const auto At = std::string("export const secret = '").size();
  auto Latin = F.Bytes;
  Latin[Base + At] = char(0xe9);
  const auto L = extractBun(input(Latin));
  const auto Accent = bunSourceRange(L, L.Modules[0], At, 2, 1024);
  EXPECT_EQ(Accent.Offset, Base + At);
  EXPECT_EQ(Accent.Size, 1);
  EXPECT_EQ(Accent.Text.substr(At, 2), "é");
  EXPECT_EQ(bunSourceRange(L, L.Modules[0], At + 2, 1, 1024).Offset,
            Base + At + 1);
  EXPECT_THROW(bunSourceRange(L, L.Modules[0], At + 1, 0, 1024), Error);
  auto UTF8 = F.Bytes;
  UTF8.replace(Base + At, 3, "中");
  UTF8.replace(Base, 7, " \r\n    ");
  put(UTF8, 4104 + F.Table + 48, 0, 1);
  put(UTF8, 4104 + F.Table + 51, 1, 1);
  const auto U = extractBun(input(UTF8));
  const auto Character = bunSourceRange(U, U.Modules[0], At, 3, 1024);
  EXPECT_EQ(Character.Offset, Base + At);
  EXPECT_EQ(Character.Size, 3);
  EXPECT_EQ(Character.Text.substr(At, 3), "中");
  EXPECT_THROW(bunSourceRange(U, U.Modules[0], At + 1, 1, 1024), Error);
  EXPECT_THROW(bunSourceRange(U, U.Modules[0], 2, 0, 1024), Error);
  const auto Newline = bunSourceRange(U, U.Modules[0], 1, 2, 1024);
  EXPECT_EQ(Newline.Offset, Base + 1);
  EXPECT_EQ(Newline.Size, 2);
}

TEST_F(WebBun, CountsAndAliasedNativeOwnersAreBoundedBeforeHashing) {
  const BunFixture F;
  auto Oversized = F.Graph.substr(0, F.Table);
  Oversized.resize(F.Table + (MaxBunModules + 1) * 52 + 4);
  const auto Footer = Oversized.size();
  Oversized += F.Graph.substr(F.Footer);
  put(Oversized, Footer, Footer, 8);
  put(Oversized, Footer + 12, (MaxBunModules + 1) * 52, 4);
  refuse(bunELF(Oversized), "bun_module_budget_exceeded");
  auto ManyBuiltins = F.Bytes;
  put(ManyBuiltins, 4104 + F.Table + 156 + 12, MaxBunBuiltins + 1, 4);
  refuse(ManyBuiltins, "bun_builtin_budget_exceeded");
  auto Alias = F.Bytes;
  const auto SH = get(Alias, 40, 8);
  Alias.replace(512, 16, Alias.substr(128, 16));
  put(Alias, SH + 64 + 24, 512, 8);
  Alias.replace(120, 56, Alias.substr(64, 56));
  put(Alias, 56, 2, 2);
  refuse(Alias, "bun_ambiguous_load_mapping");
  auto Duplicate = F.Bytes;
  Duplicate += F.Bytes.substr(SH + 128, 64);
  put(Duplicate, 60, 4, 2);
  refuse(Duplicate, "bun_duplicate_section");
}
#endif
} // namespace
