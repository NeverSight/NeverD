#include "BunSourceMap.h"
#include "BunSourceMapFixture.h"
#include "Internal.h"
#include "gtest/gtest.h"

#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::test;
class WebBunSourceMap : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    if (!bunSourceMapAvailable())
      GTEST_SKIP() << "LLVM Zstd decoding is unavailable";
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-bun-map", Directory));
    Root = Directory.str().str();
  }
  void TearDown() override {
    if (!Root.empty()) {
      std::error_code EC;
      std::filesystem::remove_all(Root, EC);
    }
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
  SourceMap decode(std::string_view Bytes) {
    // The synthetic container has a 257-byte prefix, deliberately unaligned.
    const auto A = input(std::string(257, 'x') + std::string(Bytes));
    BunExtraction E;
    E.ID = "test-extraction";
    E.ArtifactID = A.ID;
    E.Regions.push_back({"test-map-region", "source_map", sha256(Bytes), 257, 0,
                         A.Content.slice(257, Bytes.size())});
    BunModule M;
    M.ID = "test-module";
    M.SourceArtifactID = "test-generated-source";
    M.SourceMap = 0;
    return decodeBunSourceMap(E, M);
  }
  void refuse(std::string_view Bytes, const char *Code) {
    try {
      (void)decode(Bytes);
      FAIL() << "accepted " << Code;
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), Code);
    }
  }
  std::string fixture(const char *Name) {
    std::ifstream F(std::string(NEVERD_WEB_FIXTURE_DIR) + "/bun/" + Name +
                        ".graph.bin",
                    std::ios::binary);
    if (!F)
      throw std::runtime_error("missing compiler fixture");
    return {std::istreambuf_iterator<char>(F), {}};
  }
};

#ifndef _WIN32
TEST_F(WebBunSourceMap, CompilerMapsRecoverOriginalSourcesAndAnchors) {
  for (const auto Name : {"asset-map", "cache-map"}) {
    SCOPED_TRACE(Name);
    const auto E = extractBun(input(bunELF(fixture(Name))));
    const auto &M = E.Modules[0];
    const auto Map = decodeBunSourceMap(E, M);
    ASSERT_EQ(Map.Sources.size(), 1);
    ASSERT_TRUE(Map.Sources[0].Contents);
    EXPECT_EQ(Map.Profile, BunSourceMapProfile);
    EXPECT_TRUE(Map.MappedAnchorsOnly);
    EXPECT_TRUE(Map.Names.empty());
    EXPECT_EQ(Map.BlobHash, E.Regions[M.SourceMap].BlobHash);
    ASSERT_TRUE(Map.Embedding);
    EXPECT_EQ(Map.Embedding->ContainerArtifactID, E.ArtifactID);
    EXPECT_EQ(Map.Embedding->GeneratedArtifactID, M.SourceArtifactID);
    EXPECT_EQ(Map.ArtifactID, E.Regions[M.SourceMap].ID);
    EXPECT_EQ(Map.ID, decodeBunSourceMap(E, M).ID);
    const auto &Original = *Map.Sources[0].Contents;
    const auto Generated = bunSourceBytes(E, M, 1024 * 1024);
    const TextCoordinates GenCoordinates(Generated),
        OriginalCoordinates(Original);
    for (const auto &S : Map.Segments) {
      EXPECT_EQ(S.SourceIndex, 0);
      EXPECT_FALSE(S.NameIndex);
      EXPECT_NO_THROW(GenCoordinates.byteOffset(S.Generated));
      EXPECT_NO_THROW(OriginalCoordinates.byteOffset(S.Original));
    }
    if (std::string_view(Name) == "cache-map") {
      EXPECT_EQ(
          sha256(Original),
          "dae4f14862158ace4ba0cfb51115157698a5286192f7883ff89123a17b4ad687");
      ASSERT_EQ(Map.Segments.size(), 13);
      // Independently located statement boundaries in the preserved generated
      // text and original self-authored source, not decoder-produced goldens.
      EXPECT_EQ(Map.Segments[0].Generated, (TextPosition{46, 0}));
      EXPECT_EQ(Map.Segments[0].Original, (TextPosition{0, 7}));
      const auto Byte = GenCoordinates.byteOffset(Map.Segments[0].Generated);
      EXPECT_EQ(Generated.substr(Byte, 3), "var");
      EXPECT_EQ(Original.substr(7, 5), "const");
    } else {
      EXPECT_EQ(
          sha256(Original),
          "ada8ba82b03b7bd85876cc812e0108150d30465e0945ac4b0798ecdb7db31433");
      EXPECT_FALSE(Map.Segments.empty());
    }
    const auto &Storage = *Map.Sources[0].Storage;
    const auto &RawMap = E.Regions[M.SourceMap];
    EXPECT_EQ(sha256(RawMap.Content.read(Storage.ContentOffset - RawMap.Offset,
                                         Storage.ContentSize)),
              Storage.ContentHash);
    EXPECT_EQ(RawMap.Content.read(Storage.NameOffset - RawMap.Offset,
                                  Storage.NameSize),
              *Map.Sources[0].Name);
    EXPECT_NE(Storage.ContentHash, sha256(Original));
  }
}

TEST_F(WebBunSourceMap, SimpleUnicodeEmptySourcesAndEvidenceIdentities) {
  const auto Raw = simpleBunMap();
  const auto Map = decode(Raw);
  ASSERT_EQ(Map.Segments.size(), 2);
  EXPECT_EQ(Map.Segments[1].Generated, (TextPosition{0, 7}));
  EXPECT_EQ(Map.Segments[1].Original, (TextPosition{0, 7}));
  EXPECT_EQ(sourceMapAnchors(Map, {0, 8}), std::vector<uint64_t>{1});
  EXPECT_EQ(Map.Embedding->Offset, 257);
  EXPECT_NE(Map.ID, Map.ArtifactID);
  EXPECT_NE(Map.Sources[0].ID, Map.ID);
  EXPECT_NE(Map.Sources[0].Storage->ContentHash,
            sha256(*Map.Sources[0].Contents));
  auto Empty = std::string(33, '\0');
  put(Empty, 0, 33, 8);
  put(Empty, 16, 1, 8);
  put(Empty, 28, 32, 4);
  auto NoSources = decode(serializedBunMap(Empty, {}, {}));
  EXPECT_TRUE(NoSources.Sources.empty());
  EXPECT_TRUE(NoSources.Segments.empty());
  const auto Unicode = decode(serializedBunMap(
      Empty, {"中文/🌱", ""}, {rawZstd("const 中文 = '🌱';"), rawZstd("")}));
  EXPECT_EQ(*Unicode.Sources[0].Contents, "const 中文 = '🌱';");
  EXPECT_EQ(*Unicode.Sources[1].Contents, "");
  EXPECT_NE(Unicode.ID, Map.ID);
  EXPECT_EQ(
      decode(serializedBunMap(simpleMappings(), {"path"}, {repeatedZstd(500)}))
          .Sources[0]
          .Contents,
      std::string(500, 'a'));
}

TEST_F(WebBunSourceMap, RareDeltasAndDuplicateAnchorsKeepExactCoordinates) {
  // Four hand-authored states, including column reset, source change, negative
  // original deltas, equality against the rare generated-line delta and a
  // duplicate generated anchor. Names and original contents remain separate.
  std::string B(56 + 32, '\0');
  put(B, 8, 4, 8);
  put(B, 16, 6, 8);
  put(B, 24, 1, 4);
  put(B, 28, 56, 4);
  put(B, 36, 10, 4); // seed: generated (0,10), original (2,5), source 0.
  put(B, 44, 2, 4);
  put(B, 48, 5, 4);
  put(B, 56, 4, 1);
  put(B, 57, 12, 1);
  put(B, 58, 3, 2);
  put(B, 60, 1, 2);
  put(B, 62, 2, 2);
  put(B, 64, 2, 8);            // generated lines: 0,+3,0.
  put(B, 72, 6, 8);            // original lines: -1,+3,0.
  put(B, 80, 4, 8);            // original cols: -2,+10,0.
  B.append("\x04\x08\x00", 3); // generated: (0,12), (3,4), (3,4).
  B.append("\x01", 1);         // original-line exception -1.
  B.append("\x03\x14", 2);     // original-column exceptions -2, +10.
  B.append("\x01\x06\xff", 3); // delta 1 has generated-line +3.
  appendLE(B, 4, 8);           // source deltas: +1,-1,0.
  B.append("\x02\x01\0", 3);   // source exceptions, tail pad.
  put(B, 0, B.size(), 8);
  // Max original line is 4, so from_vlq's input-line field is 5.
  put(B, 16, 5, 8);
  const auto Map =
      decode(serializedBunMap(B, {"a", "b"}, {rawZstd(""), rawZstd("")}));
  ASSERT_EQ(Map.Segments.size(), 4);
  EXPECT_EQ(Map.Segments[1].Generated, (TextPosition{0, 12}));
  EXPECT_EQ(Map.Segments[1].Original, (TextPosition{1, 3}));
  EXPECT_EQ(Map.Segments[1].SourceIndex, 1);
  EXPECT_EQ(Map.Segments[2].Generated, (TextPosition{3, 4}));
  EXPECT_EQ(Map.Segments[2].Original, (TextPosition{4, 13}));
  EXPECT_EQ(Map.Segments[2].SourceIndex, 0);
  EXPECT_EQ(sourceMapAnchors(Map, {3, 4}), (std::vector<uint64_t>{2, 3}));
  auto Bad = B;
  put(Bad, 94, 255, 1); // Missing required exception, leaving excess bytes.
  EXPECT_THROW(
      decode(serializedBunMap(Bad, {"a", "b"}, {rawZstd(""), rawZstd("")})),
      Error);
}

TEST_F(WebBunSourceMap, MultipleSyncWindowsAndCountBudgets) {
  std::string B(80 + 32, '\0');
  put(B, 8, 65, 8);
  put(B, 16, 1, 8);
  put(B, 24, 2, 4);
  put(B, 28, 80, 4);
  put(B, 60, 64, 4); // second seed generated column.
  put(B, 64, 95, 4); // second window starts after header + 63 deltas.
  put(B, 72, 64, 4); // second seed original column.
  put(B, 80, 64, 1);
  put(B, 82, 63, 2);
  put(B, 96, INT64_MAX, 8);
  put(B, 104, INT64_MAX, 8);
  B.append(63, '\2');
  B.append(32, '\0');
  put(B, 175, 1, 1);
  B += '\0';
  put(B, 0, B.size(), 8);
  const auto Map = decode(serializedBunMap(B, {"a"}, {rawZstd("")}));
  ASSERT_EQ(Map.Segments.size(), 65);
  EXPECT_EQ(Map.Segments.back().Generated, (TextPosition{0, 64}));
  EXPECT_EQ(Map.Segments[63].Original, (TextPosition{0, 63}));
  put(B, 64, 94, 4);
  refuse(serializedBunMap(B, {"a"}, {rawZstd("")}),
         "bun_map_invalid_window_offset");
  put(B, 8, MaxSourceMapSegments + 1, 8);
  refuse(serializedBunMap(B, {"a"}, {rawZstd("")}),
         "source_map_segment_budget_exceeded");
}

TEST_F(WebBunSourceMap,
       HostilePointersCountsMasksVarintsAndCoordinatesFailClosed) {
  const auto Valid = simpleBunMap();
  auto Mutate = [&](uint64_t Offset, uint64_t Value, unsigned Width,
                    const char *Code) {
    auto B = Valid;
    put(B, Offset, Value, Width);
    refuse(B, Code);
  };
  Mutate(0, MaxBunMapSources + 1, 4, "source_map_source_budget_exceeded");
  Mutate(8, 0, 4, "bun_map_invalid_pointer");
  Mutate(20, UINT32_MAX, 4, "bun_map_invalid_pointer");
  Mutate(24, 89, 8, "bun_map_invalid_mapping_header");
  Mutate(24 + 16, 2, 8, "bun_map_invalid_input_line_count");
  Mutate(24 + 24, 0, 4, "bun_map_invalid_mapping_header");
  Mutate(24 + 28, 57, 4, "bun_map_invalid_mapping_header");
  Mutate(24 + 32, UINT32_MAX, 4, "bun_map_invalid_coordinate");
  Mutate(24 + 52, 1, 4, "bun_map_invalid_coordinate");
  Mutate(24 + 56, 65, 1, "bun_map_invalid_window_count");
  Mutate(24 + 57, 16, 1, "bun_map_unsupported_window_flags");
  Mutate(24 + 72, 2, 8, "bun_map_invalid_mask");
  Mutate(24 + 88, 1, 1, "bun_map_invalid_coordinate");
  Mutate(24 + 88, 128, 1, "bun_map_truncated");
  Mutate(24 + 89, 1, 1, "bun_map_invalid_mapping_padding");
  refuse(Valid + 'x', "bun_map_unconsumed_bytes");
  for (size_t N = 0; N < Valid.size(); ++N)
    EXPECT_THROW(decode(std::string_view(Valid).substr(0, N)), Error) << N;
  // Excess lane bytes, non-minimal and >32-bit varints cannot be hidden.
  for (const auto Bytes :
       {std::string("\x8e\0", 2), std::string("\x80\x80\x80\x80\x10", 5)}) {
    auto M = simpleMappings();
    M.replace(88, 1, Bytes);
    put(M, 0, M.size(), 8);
    put(M, 58, Bytes.size(), 2);
    refuse(serializedBunMap(M, {"a"}, {rawZstd("")}), "bun_map_invalid_varint");
  }
}

TEST_F(WebBunSourceMap,
       ZstdEnvelopeCorruptionAndDecodedBudgetsRefuseBeforeAllocation) {
  auto Frame = rawZstd("const x = 1;");
  auto Check = [&](std::string_view F, const char *Code) {
    refuse(serializedBunMap(simpleMappings(), {"a"}, {std::string(F)}), Code);
  };
  Check(Frame + Frame, "bun_map_unconsumed_bytes");
  Check(Frame + 'x', "bun_map_unconsumed_bytes");
  auto Bad = Frame;
  put(Bad, 0, 0x184d2a50, 4);
  Check(Bad, "bun_map_unsupported_zstd_frame");
  for (const auto Descriptor : {0xa1, 0xa8, 0xb0}) {
    Bad = Frame;
    put(Bad, 4, Descriptor, 1);
    Check(Bad, "bun_map_unsupported_zstd_frame");
  }
  Bad = Frame;
  put(Bad, 4, 0, 1);
  Check(Bad, "bun_map_unknown_zstd_content_size");
  Bad = Frame;
  put(Bad, 5, MaxBunMapSourceBytes + 1, 4);
  Check(Bad, "bun_map_decompression_budget_exceeded");
  Bad = Frame;
  put(Bad, 4, 0x80, 1);
  Bad.insert(5, 1, char(0xff));
  Check(Bad, "bun_map_decompression_budget_exceeded");
  Bad = Frame;
  put(Bad, 9, 7, 3);
  Check(Bad, "bun_map_invalid_zstd_block");
  Bad = Frame;
  put(Bad, 5, 13,
      4); // Structurally valid envelope, incorrect regenerated size.
  Check(Bad, "bun_map_invalid_zstd_data");
  Bad = Frame;
  put(Bad, 4, 0xa4, 1);
  Bad.append(4, '\0'); // Wrong content checksum.
  Check(Bad, "bun_map_invalid_zstd_data");
  Check(rawZstd(std::string("\xed\xa0\x80", 3)),
        "bun_map_invalid_source_unicode");
  refuse(serializedBunMap(simpleMappings(), {std::string("\xff", 1)}, {Frame}),
         "bun_map_invalid_name_unicode");
  const auto Large = repeatedZstd(MaxBunMapSourceBytes);
  const auto AtLimit =
      decode(serializedBunMap(simpleMappings(), {"a", "b"}, {Large, Large}));
  ASSERT_EQ(AtLimit.Sources.size(), 2);
  EXPECT_EQ(AtLimit.Sources[0].Contents->size(), MaxBunMapSourceBytes);
  EXPECT_EQ(AtLimit.Sources[1].Contents->size(), MaxBunMapSourceBytes);
  refuse(serializedBunMap(simpleMappings(), {"a", "b", "c"},
                          {Large, Large, rawZstd("x")}),
         "bun_map_decompression_budget_exceeded");
  auto TooLarge = std::string(MaxSourceMapBytes + 1, 'x');
  refuse(TooLarge, "bun_map_byte_budget_exceeded");
}
#endif
} // namespace
