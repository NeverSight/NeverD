//===- RichHeaderTests.cpp - Rich header and signature selection ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace neverd;

namespace {

constexpr uint16_t Linker = 0x0102;      // [LNK] from Visual Studio 2015 on
constexpr uint16_t CppCompiler = 0x0105; // [C++] from Visual Studio 2015 on

void put32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

uint32_t rotl(uint32_t Value, uint32_t Count) {
  Count &= 31;
  return Count == 0 ? Value : (Value << Count) | (Value >> (32 - Count));
}

/// A PE file prefix whose DOS stub holds a Rich header with \p Entries, keyed
/// with the checksum Microsoft's linker computes.
std::vector<uint8_t> fileWithRichHeader(const std::vector<RichEntry> &Entries,
                                        bool KeepChecksum = true) {
  constexpr uint32_t DanSAt = 0x80;
  const uint32_t RichAt =
      DanSAt + 16 + 8 * static_cast<uint32_t>(Entries.size());
  const uint32_t PEAt = (RichAt + 8 + 7) & ~7u;
  std::vector<uint8_t> File(PEAt + 4, 0);
  File[0] = 'M';
  File[1] = 'Z';
  for (uint32_t I = 0x40; I < DanSAt; ++I)
    File[I] = static_cast<uint8_t>(I * 7); // a DOS stub's worth of bytes
  put32(File, 0x3C, PEAt);
  File[PEAt] = 'P';
  File[PEAt + 1] = 'E';

  uint32_t Key = DanSAt;
  for (uint32_t I = 0; I < DanSAt; ++I)
    if (I < 0x3C || I >= 0x40)
      Key += rotl(File[I], I);
  for (const RichEntry &E : Entries)
    Key += rotl((uint32_t(E.ProdId) << 16) | E.Build, E.Count);
  if (!KeepChecksum)
    Key ^= 0x1000;

  put32(File, DanSAt, 0x536E6144 ^ Key);
  for (uint32_t Pad = 1; Pad <= 3; ++Pad)
    put32(File, DanSAt + 4 * Pad, Key);
  uint32_t At = DanSAt + 16;
  for (const RichEntry &E : Entries) {
    put32(File, At, ((uint32_t(E.ProdId) << 16) | E.Build) ^ Key);
    put32(File, At + 4, E.Count ^ Key);
    At += 8;
  }
  put32(File, RichAt, 0x68636952);
  put32(File, RichAt + 4, Key);
  return File;
}

TEST(RichHeader, DecodesRecordsAndChecksTheKey) {
  const std::vector<RichEntry> Entries = {
      {CppCompiler, 35403, 93}, {CppCompiler, 35738, 1}, {Linker, 35738, 1}};
  const auto Header = decodeRichHeader(fileWithRichHeader(Entries));
  ASSERT_TRUE(Header.has_value());
  EXPECT_TRUE(Header->ChecksumMatches);
  EXPECT_EQ(Header->Offset, 0x80u);
  ASSERT_EQ(Header->Entries.size(), 3u);
  EXPECT_EQ(Header->Entries[0].ProdId, CppCompiler);
  EXPECT_EQ(Header->Entries[0].Build, 35403);
  EXPECT_EQ(Header->Entries[0].Count, 93u);
  EXPECT_EQ(Header->Entries[2].ProdId, Linker);
}

TEST(RichHeader, EditedRecordsDoNotChooseReleases) {
  const auto Header =
      decodeRichHeader(fileWithRichHeader({{Linker, 36257, 1}}, false));
  ASSERT_TRUE(Header.has_value());
  EXPECT_FALSE(Header->ChecksumMatches);
  EXPECT_TRUE(richToolsetYears(*Header).empty());
}

TEST(RichHeader, FilesWithoutAWholeBlockHaveNone) {
  std::vector<uint8_t> File = fileWithRichHeader({{Linker, 36257, 1}});
  std::vector<uint8_t> NoRich = File;
  for (size_t I = 0x40; I + 4 <= NoRich.size(); ++I)
    if (llvm::support::endian::read32le(NoRich.data() + I) == 0x68636952)
      put32(NoRich, I, 0);
  EXPECT_FALSE(decodeRichHeader(NoRich).has_value());

  std::vector<uint8_t> NoDanS = File;
  put32(NoDanS, 0x80, 0);
  EXPECT_FALSE(decodeRichHeader(NoDanS).has_value());

  std::vector<uint8_t> DirtyPadding = File;
  DirtyPadding[0x84] ^= 1;
  EXPECT_FALSE(decodeRichHeader(DirtyPadding).has_value());

  EXPECT_FALSE(decodeRichHeader(std::vector<uint8_t>{'M', 'Z'}).has_value());
}

TEST(RichHeader, BuildsNameTheirRelease) {
  const RichToolInfo Exact = describeRichEntry(Linker, 36257);
  EXPECT_EQ(Exact.Tool, RichTool::Linker);
  EXPECT_EQ(Exact.VisualStudioYear, 2026u);
  EXPECT_TRUE(Exact.ExactBuild);
  EXPECT_EQ(Exact.Description, "VS2026 v18.10.0 build 36257");

  // VS 2026 14.50's linker is not listed; the nearest earlier build is.
  const RichToolInfo Unlisted = describeRichEntry(Linker, 35738);
  EXPECT_EQ(Unlisted.VisualStudioYear, 2026u);
  EXPECT_FALSE(Unlisted.ExactBuild);
  EXPECT_EQ(describeRichEntry(Linker, 24247).VisualStudioYear, 2015u);
  EXPECT_EQ(describeRichEntry(Linker, 27054).VisualStudioYear, 2017u);

  // Before 2015 each release had product ids of its own.
  const RichToolInfo OldC = describeRichEntry(0x0083, 30729);
  EXPECT_EQ(OldC.Tool, RichTool::C);
  EXPECT_EQ(OldC.VisualStudioYear, 2008u);
  // VS 2005 and VS 2012 were both build 50727; only the product id tells
  // their linkers apart.
  EXPECT_EQ(describeRichEntry(0x0078, 50727).VisualStudioYear, 2005u);
  EXPECT_EQ(describeRichEntry(0x00CC, 50727).VisualStudioYear, 2012u);
  EXPECT_EQ(describeRichEntry(0x00CC, 50727).Tool, RichTool::Linker);

  EXPECT_EQ(describeRichEntry(0xFFF0, 1).Tool, RichTool::Unknown);
  EXPECT_EQ(describeRichEntry(0xFFF0, 1).VisualStudioYear, 0u);
}

TEST(RichHeader, TheLinkerChoosesTheRelease) {
  // The runtime objects of VS 2026 14.50 were compiled with a build that
  // precedes every listed VS 2026 build; the linker's build decides.
  RichHeader Header;
  Header.ChecksumMatches = true;
  Header.Entries = {{CppCompiler, 35403, 93}, {Linker, 35738, 1}};
  EXPECT_EQ(richToolsetYears(Header), std::vector<unsigned>{2026});

  Header.Entries = {{CppCompiler, 27054, 12}};
  EXPECT_EQ(richToolsetYears(Header), std::vector<unsigned>{2017});
}

std::vector<std::filesystem::path>
names(const std::vector<std::filesystem::path> &Paths) {
  std::vector<std::filesystem::path> Names;
  for (const auto &Path : Paths)
    Names.push_back(Path.filename());
  return Names;
}

TEST(RichHeader, SelectsTheLinkersReleaseAndEveryOtherFile) {
  const std::vector<std::filesystem::path> Files = {
      "d/masm32.pat", "d/vs2013.pat", "d/vs2017.pat", "d/vs2026.pat",
      "d/winsdk.pat"};
  BinaryImage Img;
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Img, Files), Files);

  RichHeader Header;
  Header.ChecksumMatches = true;
  Header.Entries = {{Linker, 36257, 1}};
  Img.COFFRichHeader = Header;
  EXPECT_EQ(names(sigs::SignatureDB::selectForImage(Img, Files)),
            (std::vector<std::filesystem::path>{"masm32.pat", "vs2026.pat",
                                                "winsdk.pat"}));

  // A release the directory has no file for keeps every file.
  Img.COFFRichHeader->Entries = {{Linker, 30133, 1}};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Img, Files), Files);

  // So does a header that was edited after linking.
  Img.COFFRichHeader->Entries = {{Linker, 36257, 1}};
  Img.COFFRichHeader->ChecksumMatches = false;
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Img, Files), Files);
}

TEST(RichHeader, APartOfAReleasesFileIsThatRelease) {
  const std::vector<std::filesystem::path> Files = {
      "d/vs2022.pat", "d/vs2022.part2.pat", "d/vs2026.pat",
      "d/vs2026.part2.pat", "d/winsdk.pat", "d/winsdk.part2.pat"};
  BinaryImage Img;
  RichHeader Header;
  Header.ChecksumMatches = true;
  Header.Entries = {{Linker, 36257, 1}};
  Img.COFFRichHeader = Header;
  EXPECT_EQ(names(sigs::SignatureDB::selectForImage(Img, Files)),
            (std::vector<std::filesystem::path>{
                "vs2026.pat", "vs2026.part2.pat", "winsdk.pat",
                "winsdk.part2.pat"}));

  EXPECT_EQ(sigs::SignatureDB::libraryName("d/ubuntu-libc6.part12.pat"),
            "ubuntu-libc6");
  EXPECT_EQ(sigs::SignatureDB::libraryName("d/ubuntu-libc6.pat"), "ubuntu-libc6");
  EXPECT_EQ(sigs::SignatureDB::libraryName("d/x.part1.pat"), "x.part1");
  EXPECT_EQ(sigs::SignatureDB::libraryName("d/x.part02.pat"), "x.part02");
  EXPECT_EQ(sigs::SignatureDB::libraryName("d/x.partial.pat"), "x.partial");
}

} // namespace
