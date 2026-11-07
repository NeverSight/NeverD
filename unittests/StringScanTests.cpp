//===- StringScanTests.cpp - text strings in binary data ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/Limits.h"
#include "neverd/support/StringScan.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace neverd::strings;

namespace {

/// The bytes of a literal, without the NUL the compiler adds.
template <size_t N> std::vector<uint8_t> bytes(const char (&Text)[N]) {
  return std::vector<uint8_t>(Text, Text + N - 1);
}

/// \p Text as UTF-16 or UTF-32 code units with a terminator.
std::vector<uint8_t> wide(std::u32string_view Text, unsigned Unit,
                          bool BigEndian) {
  std::vector<uint8_t> Out;
  const auto put = [&](uint32_t Value) {
    for (unsigned I = 0; I < Unit; ++I) {
      const unsigned Shift = 8 * (BigEndian ? Unit - 1 - I : I);
      Out.push_back(static_cast<uint8_t>(Value >> Shift));
    }
  };
  for (char32_t C : Text) {
    if (Unit == 2 && C >= 0x10000) {
      put(0xd800 + ((C - 0x10000) >> 10));
      put(0xdc00 + ((C - 0x10000) & 0x3ff));
    } else {
      put(C);
    }
  }
  put(0);
  return Out;
}

std::vector<FoundString> scanAll(const std::vector<uint8_t> &Data,
                                 ScanOptions Options = {}) {
  std::vector<FoundString> Found;
  scan(Data, Options,
       [&](FoundString &&String) { Found.push_back(std::move(String)); });
  return Found;
}

constexpr EncodingSet Everything =
    encodingBit(Encoding::ASCII) | encodingBit(Encoding::UTF8) |
    encodingBit(Encoding::UTF16LE) | encodingBit(Encoding::UTF16BE) |
    encodingBit(Encoding::UTF32LE) | encodingBit(Encoding::UTF32BE);

TEST(StringScan, FindsASCIIAndUTF8CStrings) {
  const auto Found =
      scanAll(bytes("\x01hello\0\xff\xfe"
                    "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6\0"
                    "\xe9\x94\x99\xe8\xaf\xaf: file\0"));
  ASSERT_EQ(Found.size(), 3u);
  EXPECT_EQ(Found[0].Offset, 1u);
  EXPECT_EQ(Found[0].Kind, Encoding::ASCII);
  EXPECT_EQ(Found[0].Text, "hello");
  EXPECT_EQ(Found[1].Kind, Encoding::UTF8);
  EXPECT_EQ(Found[1].Chars, 4u);
  EXPECT_EQ(Found[1].Bytes, 12u);
  EXPECT_EQ(Found[1].Text, "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6");
  EXPECT_EQ(Found[2].Kind, Encoding::UTF8);
  EXPECT_EQ(Found[2].Chars, 8u);
}

TEST(StringScan, RejectsMalformedControlAndUnterminatedText) {
  // A truncated sequence, a C1 control (U+0085), a short string and text
  // that runs to the end of the data.
  const auto Found = scanAll(bytes("abcd\xe4\xb8\0wxyz\xc2\x85\0abc\0tail"));
  EXPECT_TRUE(Found.empty());
  ScanOptions OnlyASCII;
  OnlyASCII.Encodings = encodingBit(Encoding::ASCII);
  EXPECT_TRUE(scanAll(bytes("\xe4\xb8\xad\xe6\x96\x87"
                            "ab\0"),
                      OnlyASCII)
                  .empty());
}

TEST(StringScan, FindsWideStringsAtTheirAlignment) {
  ScanOptions Options;
  Options.Encodings = Everything;
  for (const auto &[Unit, BigEndian, Kind] :
       {std::tuple{2u, false, Encoding::UTF16LE},
        std::tuple{2u, true, Encoding::UTF16BE},
        std::tuple{4u, false, Encoding::UTF32LE},
        std::tuple{4u, true, Encoding::UTF32BE}}) {
    SCOPED_TRACE(encodingName(Kind).str());
    std::vector<uint8_t> Data(Unit, 0);
    const auto Text = wide(U"#document 中文\U0001F600", Unit, BigEndian);
    Data.insert(Data.end(), Text.begin(), Text.end());
    const auto Found = scanAll(Data, Options);
    ASSERT_EQ(Found.size(), 1u);
    EXPECT_EQ(Found[0].Offset, Unit);
    EXPECT_EQ(Found[0].Kind, Kind);
    EXPECT_EQ(Found[0].Text,
              "#document \xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80");
  }
  // One byte off the unit alignment, no wide string is read.
  std::vector<uint8_t> Shifted(1, 0);
  const auto Text = wide(U"#document", 2, false);
  Shifted.insert(Shifted.end(), Text.begin(), Text.end());
  for (const auto &String : scanAll(Shifted, Options))
    EXPECT_NE(String.Kind, Encoding::UTF16LE);
}

TEST(StringScan, FindsWideTextAfterACharacterOfOtherData) {
  // ntoskrnl's .rdata: the 0x40 that ends the object before L"HashDigest..."
  // reads, with the zero byte before it, as U+4000.  The run is no text, and
  // the string begins at its first ASCII character.
  ScanOptions Options;
  Options.Encodings = encodingBit(Encoding::UTF16LE);
  std::vector<uint8_t> Data = {0x00, 0x40};
  const auto Text = wide(U"HashDigestLength", 2, false);
  Data.insert(Data.end(), Text.begin(), Text.end());
  const auto Found = scanAll(Data, Options);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found[0].Offset, 2u);
  EXPECT_EQ(Found[0].Text, "HashDigestLength");
  // What follows such a character is text only when it is ASCII: a tab and
  // two ideographs after it are the rest of other data.
  std::vector<uint8_t> Mixed = {0x00, 0x40};
  const auto Rest = wide(U"\t刊瀆\"", 2, false);
  Mixed.insert(Mixed.end(), Rest.begin(), Rest.end());
  EXPECT_TRUE(scanAll(Mixed, Options).empty());
}

TEST(StringScan, FindsIdeographicWideTextAfterNarrowString) {
  for (const auto &[Unit, BigEndian, Kind] :
       {std::tuple{2u, false, Encoding::UTF16LE},
        std::tuple{2u, true, Encoding::UTF16BE},
        std::tuple{4u, false, Encoding::UTF32LE},
        std::tuple{4u, true, Encoding::UTF32BE}}) {
    for (bool IncludeASCII : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << encodingName(Kind).str() << " ASCII=" << IncludeASCII);
      auto Data = bytes("NeverD QA: navigation, clipboard, local variables");
      // The wide string is aligned, but the preceding whole unit is nonzero.
      while ((Data.size() + 1) % Unit)
        Data.push_back('!');
      Data.push_back(0);
      const size_t Offset = Data.size();
      const auto Text = wide(U"\u6d4b\u8bd5\u6587\u672c", Unit, BigEndian);
      Data.insert(Data.end(), Text.begin(), Text.end());
      ScanOptions Options;
      Options.Encodings = encodingBit(Kind);
      if (IncludeASCII)
        Options.Encodings |= encodingBit(Encoding::ASCII);
      const auto Found = scanAll(Data, Options);
      ASSERT_EQ(Found.size(), IncludeASCII ? 2u : 1u);
      EXPECT_EQ(Found.back().Offset, Offset);
      EXPECT_EQ(Found.back().Kind, Kind);
      EXPECT_EQ(Found.back().Text,
                "\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe6\x9c\xac");
    }
  }
}

TEST(StringScan, NarrowBoundaryDoesNotRelaxWideTextValidation) {
  ScanOptions Options;
  Options.Encodings = encodingBit(Encoding::UTF16LE);
  for (const auto Text :
       {U"\u4e02\u4e04\u4e05\u4e06", U"\ucccc\ucccc\ucccc\ucccc",
        U"\u5700\u6900\u6400\u6500"}) {
    auto Data = bytes("boundary text\0");
    const auto Tail = wide(Text, 2, false);
    Data.insert(Data.end(), Tail.begin(), Tail.end());
    EXPECT_TRUE(scanAll(Data, Options).empty());
  }
  auto Data = bytes("boundary text\0");
  const auto Tail = wide(U"\u6d4b\u8bd5\u6587\u672c", 2, false);
  Data.insert(Data.end(), Tail.begin(), Tail.end() - 2);
  EXPECT_TRUE(scanAll(Data, Options).empty());
  Data = bytes("boundary text\0!"); // one byte off the UTF-16 alignment
  Data.insert(Data.end(), Tail.begin(), Tail.end());
  EXPECT_TRUE(scanAll(Data, Options).empty());
}

TEST(StringScan, NarrowByteCoincidenceDoesNotSplitValidWideText) {
  ScanOptions Options;
  Options.Encodings = encodingBit(Encoding::UTF16LE);
  // The bytes of L"你好 " are `60 4F 7D 59 20 00`, which also look like
  // a terminated ASCII run. A valid wide string keeps its earlier boundary.
  const auto Data = wide(U"\u4f60\u597d \u4e2d\u6587\u5b57\u7b26", 2, false);
  const auto Found = scanAll(Data, Options);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found[0].Offset, 0u);
  EXPECT_EQ(Found[0].Chars, 7u);
}

TEST(StringScan, AsksIdeographicWideTextForMoreEvidence) {
  ScanOptions Options;
  Options.Encodings = encodingBit(Encoding::UTF16LE);
  const auto found = [&](std::vector<uint8_t> Data) {
    return scanAll(Data, Options).size() == 1;
  };
  // Common ideographs after a zero unit are text.
  EXPECT_TRUE(found(wide(U"\u4e2d\u6587\u5b57\u7b26", 2, false)));
  EXPECT_TRUE(found(wide(U"\ud55c\uad6d\uc5b4 \ubb38\uc790", 2, false)));
  // Rare ideographs, one character repeated (the 0xCCCC fill) and ASCII
  // read one byte off look like it but are not.
  EXPECT_FALSE(found(wide(U"\u4e02\u4e04\u4e05\u4e06", 2, false)));
  EXPECT_FALSE(found(wide(U"\ucccc\ucccc\ucccc\ucccc", 2, false)));
  EXPECT_FALSE(found(wide(U"\u5700\u6900\u6400\u6500", 2, false)));
  // Without a zero unit before it, ideographic text needs mostly ASCII.
  auto Embedded = wide(U"\u1234", 2, false);
  Embedded.resize(2);
  const auto Text = wide(U"\u4e2d\u6587\u5b57\u7b26", 2, false);
  Embedded.insert(Embedded.end(), Text.begin(), Text.end());
  EXPECT_TRUE(scanAll(Embedded, Options).empty());
  EXPECT_TRUE(found(wide(U"File \u6587\u4ef6", 2, false)));
  // A table of code points paired with one ASCII character is not text.
  EXPECT_FALSE(found(wide(U"\u1eb4H\u1ed0H\u1ee4H\u1ef8HJ", 2, false)));
  // An ASCII C string read as UTF-16 decodes to rare ideographs.
  EXPECT_TRUE(scanAll(bytes("ABCDEFGH\0\0"), Options).empty());
}

TEST(StringScan, ReadsTheChosenCodePageWhereUTF8Fails) {
  // A code page chosen is preferred: it reads first and wins.
  const auto scanIn = [](Encoding Legacy, std::vector<uint8_t> Data) {
    ScanOptions Options;
    Options.Encodings =
        encodingBit(Encoding::ASCII) | encodingBit(Encoding::UTF8);
    Options.Preferred = Legacy;
    return scanAll(Data, Options);
  };
  const struct {
    Encoding Legacy;
    std::vector<uint8_t> Data;
    const char *Text;
  } Cases[] = {
      {Encoding::GBK, bytes("\0\xd6\xd0\xce\xc4\xd7\xd6\xb7\xfb\xb4\xae\0"),
       "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6\xe4\xb8\xb2"},
      {Encoding::Big5, bytes("\0\xa4\xa4\xa4\xe5\xa6\x72\xa6\xea\0"),
       "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe4\xb8\xb2"},
      {Encoding::ShiftJIS, bytes("\0\x93\xfa\x96\x7b\x8c\xea\x81\x41\0"),
       "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x81"},
      {Encoding::EUCKR, bytes("\0\xc7\xd1\xb1\xb9\xbe\xee \xb9\xae\0"),
       "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xeb\xac\xb8"},
      {Encoding::Windows1252, bytes("\0caf\xe9 cr\xe8me\0"),
       "caf\xc3\xa9 cr\xc3\xa8me"},
      {Encoding::Windows1251, bytes("\0\xcf\xf0\xe8\xe2\xe5\xf2\0"),
       "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(encodingName(Case.Legacy).str());
    const auto Found = scanIn(Case.Legacy, Case.Data);
    ASSERT_EQ(Found.size(), 1u);
    EXPECT_EQ(Found[0].Kind, Case.Legacy);
    EXPECT_EQ(Found[0].Offset, 1u);
    EXPECT_EQ(Found[0].Bytes, Case.Data.size() - 2);
    EXPECT_EQ(Found[0].Text, Case.Text);
  }
  // A euro sign is not East Asian text, and no word is cased "ICÆo".
  EXPECT_TRUE(scanIn(Encoding::GBK, bytes("\0\x80wrq\0")).empty());
  EXPECT_TRUE(scanIn(Encoding::Windows1252, bytes("\0IC\xc6o\0")).empty());
  EXPECT_TRUE(
      scanIn(Encoding::Windows1251, bytes("\0\xd8\xee\xff\xff \0")).empty());
  // Fill bytes, symbols in a single-byte code page, and a run that does not
  // follow a NUL are not legacy strings; valid UTF-8 stays UTF-8.
  EXPECT_TRUE(
      scanIn(Encoding::Windows1252, bytes("\0\xcc\xcc\xcc\xcc\0")).empty());
  EXPECT_TRUE(
      scanIn(Encoding::Windows1252, bytes("\0\xa9\xae\xb1\xb5\0")).empty());
  EXPECT_TRUE(
      scanIn(Encoding::GBK, bytes("\x01\xd6\xd0\xce\xc4\xd7\xd6\xb7\xfb\0"))
          .empty());
  const auto UTF8 = scanIn(Encoding::GBK, bytes("\0\xe4\xb8\xad\xe6\x96\x87"
                                                "\xe5\xad\x97\xe7\xac\xa6\0"));
  ASSERT_EQ(UTF8.size(), 1u);
  EXPECT_EQ(UTF8[0].Kind, Encoding::UTF8);
}

TEST(StringScan, ReadsEveryCodePageSearchedInOnePass) {
  // GBK, Shift-JIS and windows-1252 strings in one pool, as the defaults
  // search them; the three ideographs of 日本語 are six columns.
  const auto Found =
      scanAll(bytes("\0\xd6\xd0\xce\xc4\xd7\xd6\xb7\xfb\xb4\xae\0"
                    "\x93\xfa\x96\x7b\x8c\xea\0"
                    "caf\xe9 cr\xe8me\0"));
  ASSERT_EQ(Found.size(), 3u);
  EXPECT_EQ(Found[0].Kind, Encoding::GBK);
  EXPECT_EQ(Found[0].Text,
            "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6\xe4\xb8\xb2");
  EXPECT_EQ(Found[1].Kind, Encoding::ShiftJIS);
  EXPECT_EQ(Found[1].Offset, 12u);
  EXPECT_EQ(Found[1].Text, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
  EXPECT_EQ(Found[2].Kind, Encoding::Windows1252);
  EXPECT_EQ(Found[2].Text, "caf\xc3\xa9 cr\xc3\xa8me");
}

TEST(StringScan, PrefersAReadingInTheCodePagesOwnScript) {
  // GBK reads the Big5 bytes of 中文檔 as いゅ郎, kana that Chinese text in
  // GBK seldom holds; Big5 reads them as the ideographs it is made for.
  const auto Data = bytes("\0\xa4\xa4\xa4\xe5\xc0\xc9\0");
  const auto Found = scanAll(Data);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found[0].Kind, Encoding::Big5);
  EXPECT_EQ(Found[0].Text, "\xe4\xb8\xad\xe6\x96\x87\xe6\xaa\x94");
  // The preferred code page reads first and wins outright.
  ScanOptions Options;
  Options.Preferred = Encoding::GBK;
  const auto Preferred = scanAll(Data, Options);
  ASSERT_EQ(Preferred.size(), 1u);
  EXPECT_EQ(Preferred[0].Kind, Encoding::GBK);
  EXPECT_EQ(Preferred[0].Text, "\xe3\x81\x84\xe3\x82\x85\xe9\x83\x8e");
}

TEST(StringScan, LeavesAStringAnotherScriptsCodePageReads) {
  // windows-1253 reads the windows-1251 bytes of Привет as Οπθβες, so
  // searched without a preference, neither is shown.
  ScanOptions Options;
  Options.Encodings = encodingBit(Encoding::Windows1251);
  EXPECT_TRUE(scanAll(bytes("\0\xcf\xf0\xe8\xe2\xe5\xf2\0"), Options).empty());
}

TEST(StringScan, SearchesAnotherCodePageOnlyWhenChosen) {
  // KOI8-R reads привет; no code page searched by default reads it.
  const auto Data = bytes("\0\xd0\xd2\xc9\xd7\xc5\xd4\0");
  EXPECT_TRUE(scanAll(Data).empty());
  ScanOptions Options;
  Options.Preferred = Encoding::KOI8R;
  const auto Found = scanAll(Data, Options);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found[0].Kind, Encoding::KOI8R);
  EXPECT_EQ(Found[0].Text, "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
}

TEST(StringScan, MeasuresTheMinimumLengthInColumns) {
  // Three ideographs fill six columns; three ASCII letters three.
  const auto Found =
      scanAll(bytes("\0\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\0abc\0"));
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found[0].Kind, Encoding::UTF8);
  EXPECT_EQ(Found[0].Chars, 3u);
  ScanOptions Seven;
  Seven.MinLength = 7;
  EXPECT_TRUE(scanAll(bytes("\0\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\0"), Seven)
                  .empty());
}

TEST(StringScan, FindsUTF32ByDefault) {
  // wchar_t is 32 bits outside Windows: L"..." literals are UTF-32LE.
  std::vector<uint8_t> Data(4, 0);
  const auto Text = wide(U"UTF32 \u4e2d\u6587", 4, false);
  Data.insert(Data.end(), Text.begin(), Text.end());
  const auto Found = scanAll(Data);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found[0].Kind, Encoding::UTF32LE);
  EXPECT_EQ(Found[0].Text, "UTF32 \xe4\xb8\xad\xe6\x96\x87");
}

TEST(StringScan, DecodesCharactersForDisplay) {
  std::vector<DecodedCharacter> Characters;
  decode(bytes("a\xd6\xd0\xff\x80"), Encoding::GBK,
         [&](const DecodedCharacter &C) { Characters.push_back(C); });
  ASSERT_EQ(Characters.size(), 4u);
  EXPECT_EQ(Characters[0].Code[0], uint32_t('a'));
  EXPECT_EQ(Characters[1].Offset, 1u);
  EXPECT_EQ(Characters[1].Bytes, 2u);
  EXPECT_EQ(Characters[1].Code[0], 0x4e2du);
  // 0xFF does not decode; 0x80 is the euro sign in GBK.
  EXPECT_EQ(Characters[2].Codes, 0u);
  EXPECT_EQ(Characters[3].Code[0], 0x20acu);
  Characters.clear();
  decode(wide(U"\U0001F600", 2, false), Encoding::UTF16LE,
         [&](const DecodedCharacter &C) { Characters.push_back(C); });
  ASSERT_EQ(Characters.size(), 2u);
  EXPECT_EQ(Characters[0].Bytes, 4u);
  EXPECT_EQ(Characters[0].Code[0], 0x1f600u);
  EXPECT_TRUE(isShownCharacter(0x4e2d));
  EXPECT_FALSE(isShownCharacter(0x202e));
  EXPECT_FALSE(isShownCharacter(0xe000));
}

TEST(StringScan, ReadsAStringFromTheCharacterAtOrAfterAByte) {
  // A reference into a string reads it from the first whole character.
  const auto UTF8 = bytes("\xe4\xb8\xad\xe6\x96\x87"
                          "abc");
  const auto Whole = textFrom(UTF8, Encoding::UTF8, 0);
  ASSERT_TRUE(Whole);
  EXPECT_EQ(Whole->Byte, 0u);
  EXPECT_EQ(Whole->UTF8, 0u);
  EXPECT_EQ(Whole->Chars, 5u);
  const auto Inside = textFrom(UTF8, Encoding::UTF8, 1);
  ASSERT_TRUE(Inside);
  EXPECT_EQ(Inside->Byte, 3u);
  EXPECT_EQ(Inside->UTF8, 3u);
  EXPECT_EQ(Inside->Chars, 4u);
  EXPECT_FALSE(textFrom(UTF8, Encoding::UTF8, UTF8.size()));
  // An odd byte of a UTF-16 string reads from the next unit.
  auto WideBytes = wide(U"Wide", 2, false);
  WideBytes.resize(8); // Without the terminator.
  const auto Wide = textFrom(WideBytes, Encoding::UTF16LE, 3);
  ASSERT_TRUE(Wide);
  EXPECT_EQ(Wide->Byte, 4u);
  EXPECT_EQ(Wide->UTF8, 2u);
  EXPECT_EQ(Wide->Chars, 2u);
  // A trail byte of a GBK character reads from the character after it,
  // whose text follows three UTF-8 bytes of the one before.
  const auto GBK = textFrom(bytes("a\xd6\xd0\xce\xc4"), Encoding::GBK, 2);
  ASSERT_TRUE(GBK);
  EXPECT_EQ(GBK->Byte, 3u);
  EXPECT_EQ(GBK->UTF8, 4u);
  EXPECT_EQ(GBK->Chars, 1u);
}

TEST(StringScan, FoldsCaseInEveryScript) {
  EXPECT_EQ(foldCase("Hello WORLD"), "hello world");
  // Cyrillic, Greek with its final sigma, Latin beyond ASCII, fullwidth.
  EXPECT_EQ(foldCase("\xd0\x9f\xd0\xa0\xd0\x98\xd0\x92\xd0\x95\xd0\xa2"),
            "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
  EXPECT_EQ(foldCase("\xce\xa3\xce\xbf\xcf\x82"), "\xcf\x83\xce\xbf\xcf\x83");
  EXPECT_EQ(foldCase("\xc3\x84\xc3\x96\xc3\x9c"), "\xc3\xa4\xc3\xb6\xc3\xbc");
  EXPECT_EQ(foldCase("\xef\xbc\xa1"), "\xef\xbd\x81");
  // Text without case, and bytes that are not UTF-8, stay.
  EXPECT_EQ(foldCase("\xe4\xb8\xad\xe6\x96\x87"), "\xe4\xb8\xad\xe6\x96\x87");
  EXPECT_EQ(foldCase("A\xff"
                     "B"),
            "a\xff"
            "b");
}

TEST(StringScan, OverlappingStringsKeepTheFirst) {
  ScanOptions Options;
  Options.Encodings = Everything;
  // "abcd" as a C string, then a UTF-16LE string right after its NUL.
  auto Data = bytes("abcd\0\0");
  const auto Text = wide(U"wide text", 2, false);
  Data.insert(Data.end(), Text.begin(), Text.end());
  const auto Found = scanAll(Data, Options);
  ASSERT_EQ(Found.size(), 2u);
  EXPECT_EQ(Found[0].Kind, Encoding::ASCII);
  EXPECT_EQ(Found[1].Kind, Encoding::UTF16LE);
  EXPECT_EQ(Found[1].Offset, 6u);
}

/// Text in the encodings the scan reads, among data it must not read as
/// text, laid out a little differently by \p Variant: wide strings off
/// their alignment, a character of other data before one, a code page string
/// after a NUL, an unterminated run.
std::vector<uint8_t> mixedText(unsigned Variant) {
  std::vector<uint8_t> Out;
  const auto add = [&](const std::vector<uint8_t> &Bytes) {
    Out.insert(Out.end(), Bytes.begin(), Bytes.end());
  };
  add(bytes("\x01\x02hello world\0"));
  add(bytes("\xc3\xa9t\xc3\xa9 au caf\xc3\xa9\0"));
  // "中文字符串" in GBK.
  add(bytes("\0\xd6\xd0\xce\xc4\xd7\xd6\xb7\xfb\xb4\xae\0"));
  if (Variant % 2)
    Out.push_back(0x90);
  add(bytes("\0\x40"));
  add(wide(U"HashDigestLength", 2, false));
  add(wide(U"\u4e2d\u6587\u5b57\u7b26\u4e32 text", 2, Variant % 3 == 1));
  add(wide(U"utf-32 text \U0001F600", 4, Variant % 3 == 2));
  add(bytes("\x48\x8b\x05\x10\x20\x30\x40\xe8\0\0\0\0\xff\x15unterminated"));
  for (unsigned I = 0; I < Variant % 7; ++I)
    Out.push_back(static_cast<uint8_t>('A' + I));
  return Out;
}

void expectSameStrings(const std::vector<FoundString> &Expected,
                       const std::vector<FoundString> &Actual) {
  ASSERT_EQ(Expected.size(), Actual.size());
  for (size_t I = 0; I < Expected.size(); ++I) {
    EXPECT_EQ(Expected[I].Offset, Actual[I].Offset) << I;
    EXPECT_EQ(Expected[I].Bytes, Actual[I].Bytes) << I;
    EXPECT_EQ(Expected[I].Chars, Actual[I].Chars) << I;
    EXPECT_EQ(Expected[I].Kind, Actual[I].Kind) << I;
    EXPECT_EQ(Expected[I].Text, Actual[I].Text) << I;
  }
}

TEST(StringScan, ReadsAtAByteWhatTheScanReportsFirstThere) {
  ScanOptions Options;
  Options.Encodings |= Everything;
  Options.MinLength = 1;
  std::vector<uint8_t> Data;
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    const auto Text = mixedText(Variant);
    Data.insert(Data.end(), Text.begin(), Text.end());
  }
  const llvm::ArrayRef<uint8_t> All(Data);
  size_t Read = 0;
  for (size_t At = 0; At < Data.size(); ++At) {
    const auto Window = All.slice(At, std::min<size_t>(256, Data.size() - At));
    std::optional<FoundString> First;
    scan(Window, Options, [&](FoundString &&Found) {
      if (!First)
        First = std::move(Found);
    });
    const auto There = scanAt(Window, Options);
    ASSERT_EQ(There.has_value(), First && First->Offset == 0) << At;
    if (!There)
      continue;
    ++Read;
    expectSameStrings({*First}, {*There});
  }
  EXPECT_GT(Read, Data.size() / 4);
}

TEST(StringScan, ReportsTheSameStringsFromSpansRunAsTasks) {
  ScanOptions Options;
  Options.Encodings |= Everything;
  std::vector<uint8_t> Data;
  for (unsigned Variant = 0;
       Data.size() < 2 * neverd::limits::support::kStringScanParallelBytes;
       ++Variant) {
    const auto Text = mixedText(Variant);
    Data.insert(Data.end(), Text.begin(), Text.end());
  }
  const auto Serial = scanAll(Data, Options);
  ASSERT_FALSE(Serial.empty());
  const auto collect = [&](ParallelTasks Parallel) {
    std::vector<FoundString> Found;
    scan(
        Data, Options,
        [&](FoundString &&String) { Found.push_back(std::move(String)); },
        Parallel);
    return Found;
  };
  // One at a time, last first: an order no serial scan takes.
  size_t Tasks = 0;
  expectSameStrings(
      Serial, collect([&](size_t Count, llvm::function_ref<void(size_t)> Task) {
        Tasks = std::max(Tasks, Count);
        for (size_t I = Count; I-- > 0;)
          Task(I);
      }));
  EXPECT_GT(Tasks, 1u);
  // Several at once.
  expectSameStrings(
      Serial, collect([](size_t Count, llvm::function_ref<void(size_t)> Task) {
        std::atomic<size_t> Next{0};
        std::vector<std::thread> Threads;
        for (unsigned T = 0; T < 4; ++T)
          Threads.emplace_back([&] {
            for (size_t I; (I = Next.fetch_add(1)) < Count;)
              Task(I);
          });
        for (std::thread &Thread : Threads)
          Thread.join();
      }));
}

TEST(StringScan, NamesEncodings) {
  EXPECT_EQ(encodingNamed("UTF-16le"), Encoding::UTF16LE);
  EXPECT_EQ(encodingNamed("cp936"), Encoding::GBK);
  EXPECT_EQ(encodingSpelling(Encoding::ShiftJIS), "Shift-JIS");
  EXPECT_TRUE(isLegacyEncoding(Encoding::Windows1252));
  EXPECT_FALSE(isLegacyEncoding(Encoding::UTF8));
  EXPECT_EQ(encodingSpelling(Encoding::UTF32BE), "UTF-32BE");
  EXPECT_TRUE(encodingSpelling(Encoding::ASCII).empty());
  EXPECT_FALSE(encodingNamed("ebcdic"));
}

} // namespace
