//===- SourceMapTests.cpp - Source map evidence tests ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source map evidence tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Session.h"
#include "neverd/web/SourceLocation.h"
#include "neverd/web/SourceMap.h"

namespace {
using namespace neverd::web;

TEST(WebCoordinates, Utf8AndUtf16RoundTripWithoutNormalizingOriginalBytes) {
  const std::string Text = "\xEF\xBB\xBF"
                           "a\xF0\x9F\x98\x80\r\n"
                           "\xCE\xB2\rc\xE2\x80\xA8"
                           "d\xE2\x80\xA9";
  TextCoordinates Coordinates(Text);
  ASSERT_EQ(Coordinates.lineCount(), 5);
  struct Case {
    uint64_t Byte, Line, Column;
  };
  for (const auto C : {Case{0, 0, 0},
                       {3, 0, 1},
                       {4, 0, 2},
                       {8, 0, 4},
                       {10, 1, 0},
                       {12, 1, 1},
                       {13, 2, 0},
                       {14, 2, 1},
                       {17, 3, 0},
                       {18, 3, 1},
                       {21, 4, 0}}) {
    const auto Position = Coordinates.position(C.Byte);
    EXPECT_EQ(Position.Line, C.Line);
    EXPECT_EQ(Position.UTF16Column, C.Column);
    EXPECT_EQ(Coordinates.byteOffset({C.Line, C.Column}), C.Byte);
  }
  for (const auto Byte : {1, 2, 5, 6, 7, 9, 11, 15, 16, 19, 20, 22})
    EXPECT_THROW(Coordinates.position(Byte), Error);
  EXPECT_THROW(Coordinates.byteOffset({0, 3}), Error);
  EXPECT_THROW(Coordinates.byteOffset({0, 5}), Error);
  EXPECT_THROW(Coordinates.byteOffset({5, 0}), Error);
}

TEST(WebCoordinates, SparseCheckpointsDoNotSplitSurrogatePairs) {
  const auto Text =
      std::string(128, 'a') + "\xF0\x9F\x98\x80" + std::string(128, 'b');
  const TextCoordinates Coordinates(Text);
  EXPECT_EQ(Coordinates.byteOffset({0, 128}), 128);
  EXPECT_THROW(Coordinates.byteOffset({0, 129}), Error);
  EXPECT_EQ(Coordinates.byteOffset({0, 130}), 132);
  EXPECT_EQ(Coordinates.position(Text.size()).UTF16Column, 258);
  EXPECT_THROW(TextCoordinates(std::string("\xC0\x80", 2)), Error);
  const TextCoordinates Empty("");
  EXPECT_EQ(Empty.lineCount(), 1);
  EXPECT_EQ(Empty.byteOffset({0, 0}), 0);
}

std::string mapText(std::string_view Mappings) {
  return R"({"version":3,"sources":["source.js"],"names":["value"],"mappings":")" +
         std::string(Mappings) + R"("})";
}

TEST(WebSourceMaps, DeltaStateAndUnmappedAnchorsRemainDistinct) {
  const auto Map =
      decodeSourceMap("map-occurrence", mapText("AAAAA,CAAC,E;AACD"));
  ASSERT_EQ(Map.Segments.size(), 4);
  EXPECT_EQ(Map.Segments[0].NameIndex, 0);
  EXPECT_EQ(Map.Segments[1].Generated.UTF16Column, 1);
  EXPECT_EQ(Map.Segments[1].Original.UTF16Column, 1);
  EXPECT_FALSE(Map.Segments[1].NameIndex);
  EXPECT_FALSE(Map.Segments[2].SourceIndex);
  EXPECT_EQ(Map.Segments[3].Generated.Line, 1);
  EXPECT_EQ(Map.Segments[3].Generated.UTF16Column, 0);
  EXPECT_EQ(Map.Segments[3].Original.Line, 1);
  EXPECT_EQ(Map.Segments[3].Original.UTF16Column, 0);
  EXPECT_EQ(sourceMapAnchors(Map, {0, 2}), (std::vector<uint64_t>{1}));
  EXPECT_EQ(sourceMapAnchors(Map, {0, 3}), (std::vector<uint64_t>{2}));
  EXPECT_TRUE(sourceMapAnchors(Map, {2, 0}).empty());
  EXPECT_NE(
      Map.ID,
      decodeSourceMap("other-occurrence", mapText("AAAAA,CAAC,E;AACD")).ID);
}

TEST(WebSourceMaps, BackwardDeltasSortWithoutRecomputingOrInventingMappings) {
  const auto Map = decodeSourceMap("map", mapText("IAAE,FAAD,AAAA"));
  ASSERT_EQ(Map.Segments.size(), 3);
  EXPECT_EQ(Map.Segments[0].Generated.UTF16Column, 2);
  EXPECT_EQ(Map.Segments[0].Original.UTF16Column, 1);
  EXPECT_EQ(Map.Segments[2].Generated.UTF16Column, 4);
  EXPECT_EQ(Map.Segments[2].Original.UTF16Column, 2);
  EXPECT_EQ(sourceMapAnchors(Map, {0, 3}), (std::vector<uint64_t>{0, 1}));
  EXPECT_TRUE(sourceMapAnchors(Map, {0, 1}).empty());
}

TEST(WebSourceMaps, IndexedOffsetsApplyColumnsOnlyOnTheFirstLine) {
  const auto Map = decodeSourceMap("map", R"({
    "version":3, "sourceRoot":"do-not-inherit", "sections":[
      {"offset":{"line":3,"column":7},"map":{
        "version":3,"sources":["same.js"],"sourcesContent":["a\nb\nc"],
        "mappings":"AACA;AACA"}},
      {"offset":{"line":5,"column":2},"map":{
        "version":3,"sourceRoot":"../private","sources":["same.js"],
        "sourcesContent":["different"],"mappings":"AAAA"}}
    ]})");
  ASSERT_TRUE(Map.Indexed);
  ASSERT_EQ(Map.Segments.size(), 3);
  EXPECT_EQ(Map.Segments[0].Generated, (TextPosition{3, 7}));
  EXPECT_EQ(Map.Segments[1].Generated, (TextPosition{4, 0}));
  EXPECT_EQ(Map.Segments[2].Generated, (TextPosition{5, 2}));
  ASSERT_EQ(Map.Sources.size(), 2);
  EXPECT_NE(Map.Sources[0].ID, Map.Sources[1].ID);
  EXPECT_FALSE(Map.Sources[0].Root);
  ASSERT_TRUE(Map.Sources[1].Root);
  EXPECT_EQ(*Map.Sources[1].Root, "../private");
  EXPECT_EQ(Map.Segments[2].SourceIndex, 1);
}

TEST(WebSourceMaps, UnknownNamesAndSensitiveReferencesArePreservedWithoutIO) {
  const auto Map = decodeSourceMap("map", R"({"version":3,
    "sources":[null,"file:///outside/SECRET"],"sourcesContent":["a",null],
    "ignoreList":[1],"mappings":"AAAA,CCAA","unknownExtension":{"safe":true}})");
  ASSERT_EQ(Map.Sources.size(), 2);
  EXPECT_FALSE(Map.Sources[0].Name);
  EXPECT_EQ(Map.Sources[0].Contents, "a");
  EXPECT_EQ(Map.Sources[1].Name, "file:///outside/SECRET");
  EXPECT_FALSE(Map.Sources[1].Contents);
  EXPECT_TRUE(Map.Sources[1].Ignored);
}

TEST(WebSourceMaps, InvalidVlqShapeAndIndexesNeverPublishPartialMappings) {
  for (const auto Text : {"AA", "AAA", "AAAAAA", "g", "///////", "gggggggA",
                          "B", "ACAA", "AAAD", "AAAAC", ",A", "A,", "A,;"})
    EXPECT_THROW(decodeSourceMap("map", mapText(Text)), Error) << Text;
  EXPECT_THROW(
      decodeSourceMap(
          "map", R"({"version":3,"version":3,"sources":[],"mappings":""})"),
      Error);
  EXPECT_THROW(
      decodeSourceMap("map", R"({"version":2,"sources":[],"mappings":""})"),
      Error);
  EXPECT_THROW(
      decodeSourceMap("map", R"({"version":3,"sources":[],"mappings":"AAAA"})"),
      Error);
}

TEST(WebSourceMaps, ExternalAndOverlappingSectionsAreRefused) {
  EXPECT_THROW(decodeSourceMap("map", R"({"version":3,"sections":[{
    "offset":{"line":0,"column":0},"url":"https://never-fetch.invalid/map"}]})"),
               Error);
  EXPECT_THROW(decodeSourceMap("map", R"({"version":3,"sections":[
    {"offset":{"line":0,"column":0},"map":{"version":3,"sources":[],"mappings":"A;A"}},
    {"offset":{"line":1,"column":0},"map":{"version":3,"sources":[],"mappings":""}}
  ]})"),
               Error);
}

TEST(WebSourceMaps, SegmentBudgetIncludesUnmappedEntries) {
  std::string Mappings = "A";
  for (uint64_t I = 0; I < MaxSourceMapSegments; ++I)
    Mappings += ",A";
  EXPECT_THROW(decodeSourceMap("map", mapText(Mappings)), Error);
}
} // namespace
