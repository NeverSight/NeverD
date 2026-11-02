//===- PatchJSONTests.cpp - Patch row reader compatibility ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Typed row ownership, JSON compatibility and persistence admission limits.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/patch/PatchIO.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace neverd::patch;

namespace {
std::string row(llvm::StringRef Size = "1", llvm::StringRef Extra = "") {
  return ("{\"addr\":\"0x2\",\"file_off\":\"0x2\",\"va\":\"0x1002\","
          "\"original\":\"cc\",\"current\":\"90\",\"kind\":\"bytes\","
          "\"size\":" +
          Size + Extra + "}")
      .str();
}

void expectRejected(llvm::StringRef Text, llvm::StringRef Message = {}) {
  auto Rows = parsePatchRows(Text);
  ASSERT_FALSE(bool(Rows)) << Text.take_front(256).str();
  const auto Error = llvm::toString(Rows.takeError());
  if (!Message.empty())
    EXPECT_EQ(Error, Message.str());
}

TEST(PatchJSON, PreservesRowOrderOptionalKeysAndEscapedFields) {
  auto Rows = parsePatchRows(
      R"([{"file_off":"0xFFFFFFFFFFFFFFFF","va":"0xffffffffffffffff",
           "original":"CCa0","current":"90A1","kind":"nop","size":2},
          {"\u0066ile_off":"0x2","addr":"0x2","va":"0x1002",
           "original":"\u0063c","current":"90","kind":"by\u0074es","size":1}])");
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  ASSERT_EQ(Rows->size(), 2u);
  EXPECT_EQ((*Rows)[0].FileOffset, UINT64_MAX);
  EXPECT_EQ((*Rows)[0].Address, UINT64_MAX);
  EXPECT_EQ((*Rows)[0].Kind, PatchKind::Nop);
  EXPECT_EQ((*Rows)[0].Original, (std::vector<uint8_t>{0xcc, 0xa0}));
  EXPECT_EQ((*Rows)[0].Current, (std::vector<uint8_t>{0x90, 0xa1}));
  EXPECT_EQ((*Rows)[1].FileOffset, 2u);
  EXPECT_EQ((*Rows)[1].Address, 0x1002u);
  EXPECT_EQ((*Rows)[1].Kind, PatchKind::Bytes);
  EXPECT_EQ((*Rows)[1].Original, (std::vector<uint8_t>{0xcc}));
}

TEST(PatchJSON, LastDecodedDuplicateKeyReplacesEarlierTypeAndValue) {
  for (llvm::StringRef Key :
       {"addr", "file_off", "va", "original", "current", "kind", "size"}) {
    SCOPED_TRACE(Key.str());
    auto Valid = parsePatchRows(
        ("[{\"" + Key + "\":null," + llvm::StringRef(row()).drop_front() + "]")
            .str());
    ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
    ASSERT_EQ(Valid->size(), 1u);
    expectRejected("[" + row("1", ",\"" + Key.str() + "\":null") + "]");
  }
  auto Rows = parsePatchRows(
      "[" +
      row("1", R"(,"k\u0069nd":"nop","kind":"bytes","size":1.0,"size":1)") +
      "]");
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  EXPECT_EQ(Rows->front().Kind, PatchKind::Bytes);
  expectRejected("[" + row("1", R"(,"k\u0069nd":"unsupported")") + "]",
                 "invalid patch row fields");
}

TEST(PatchJSON, RetainsLegacyIntegerAdmissionWithoutRoundingDecimals) {
  for (llvm::StringRef Size : {"1", "01", "+1"}) {
    SCOPED_TRACE(Size.str());
    auto Rows = parsePatchRows("[" + row(Size) + "]");
    ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
    EXPECT_EQ(Rows->front().size(), 1u);
  }
  for (llvm::StringRef Size :
       {"0", "-0", "-1", "1.0", "1e0", "true", "null", "\"1\"", "[]", "{}",
        "1e9999", "18446744073709551615", "18446744073709551616"}) {
    SCOPED_TRACE(Size.str());
    expectRejected("[" + row(Size) + "]", "invalid patch row fields");
  }
}

TEST(PatchJSON, AddressFieldsKeepUnsignedHexValuesAndLeadingZeroes) {
  const auto Check = [](llvm::StringRef Spelling, uint64_t Expected) {
    SCOPED_TRACE(Spelling.str());
    const auto Quoted = llvm::formatv("{0}", llvm::json::Value(Spelling)).str();
    auto Rows = parsePatchRows("[" +
                               row("1", ",\"file_off\":" + Quoted + ",\"va\":" +
                                            Quoted + ",\"addr\":" + Quoted) +
                               "]");
    ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
    ASSERT_EQ(Rows->size(), 1u);
    EXPECT_EQ(Rows->front().FileOffset, Expected);
    EXPECT_EQ(Rows->front().Address, Expected);
  };
  Check("0x0", 0);
  Check("0x000000000000000000000000000002", 2);
  Check("0xABCDEF", 0xabcdef);
  Check("0x" + std::string(8192, '0'), 0);
  Check("0xfFfFfFfFfFfFfFfF", UINT64_MAX);
  Check("0x" + std::string(4096, '0') + "ffffffffffffffff", UINT64_MAX);

  // Equal numeric keys need not have identical spelling.
  auto Rows = parsePatchRows(
      "[" + row("1", R"(,"file_off":"0x000a","addr":"0xA")") + "]");
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  ASSERT_EQ(Rows->size(), 1u);
  EXPECT_EQ(Rows->front().FileOffset, 10u);
  EXPECT_EQ(Rows->front().Address, 0x1002u);
}

TEST(PatchJSON, AddressFieldsRejectPartialSignedAndOverflowingHex) {
  const std::vector<std::string> Invalid{"",
                                         "0x",
                                         "0X2",
                                         " 0x2",
                                         "0x+2",
                                         "0x-0",
                                         "0x-2",
                                         "0x 2",
                                         "0x2 ",
                                         "0x2.0",
                                         "0x2p1",
                                         "0x2g",
                                         "0x0x2",
                                         "0x2\n",
                                         std::string("0x2\0", 4),
                                         "0x2\xc3\xa9",
                                         "0x10000000000000000",
                                         "0x00010000000000000000",
                                         "0xFFFFFFFFFFFFFFFFF",
                                         "0x" + std::string(8192, 'f'),
                                         "0x" + std::string(8192, '0') +
                                             std::string(17, 'f')};
  for (const auto &Spelling : Invalid) {
    SCOPED_TRACE(Spelling);
    const auto Quoted = llvm::formatv("{0}", llvm::json::Value(Spelling)).str();
    for (llvm::StringRef Key : {"file_off", "va", "addr"}) {
      SCOPED_TRACE(Key.str());
      expectRejected("[" + row("1", ",\"" + Key.str() + "\":" + Quoted) + "]",
                     Key == "addr" ? "patch row key differs from file offset"
                                   : "invalid patch row fields");
    }
  }
}

TEST(PatchJSON, OwnsDecodedBytesBeyondInputAndParserLifetime) {
  auto Rows = [] {
    std::string Text = "[" + row() + "]";
    auto Parsed = parsePatchRows(Text);
    std::fill(Text.begin(), Text.end(), '?');
    return Parsed;
  }();
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  // A subsequent parse can reuse a freed tape or input allocation.
  auto Other = parsePatchRows(
      "[" + row("1", R"(,"original":"00","current":"ff")") + "]");
  ASSERT_TRUE(bool(Other)) << llvm::toString(Other.takeError());
  EXPECT_EQ(Rows->front().Original, (std::vector<uint8_t>{0xcc}));
  EXPECT_EQ(Rows->front().Current, (std::vector<uint8_t>{0x90}));
  EXPECT_EQ(Rows->front().Address, 0x1002u);
}

TEST(PatchJSON, RejectsInvalidFieldsWithoutGuessingPatchKinds) {
  for (llvm::StringRef Extra :
       {R"(,"file_off":"0X2")", R"(,"va":"0x")",
        R"(,"va":"0x10000000000000000")", R"(,"original":[])",
        R"(,"current":{})", R"(,"kind":"unsupported")"})
    expectRejected("[" + row("1", Extra) + "]", "invalid patch row fields");
  expectRejected("[" + row("1", R"(,"addr":"0x3")") + "]",
                 "patch row key differs from file offset");
  expectRejected("[" + row("1", R"(,"original":"c")") + "]",
                 "hex bytes need pairs of digits within the byte limit");
  expectRejected("[" + row("1", R"(,"original":"xz","current":"yz")") + "]",
                 "invalid hexadecimal byte");
  expectRejected("[" + row("2") + "]",
                 "patch row size does not match its bytes");
  expectRejected("[{}]", "invalid patch row fields");
  expectRejected("[null]", "patch row must be an object");
  expectRejected("{}", "patches.json must be a bounded array");
}

TEST(PatchJSON, UnknownFieldsKeepLegacyParsingAndCannotHideSyntaxErrors) {
  auto Rows = parsePatchRows(
      "[" +
      row("1",
          R"(,"extra":{"a":1,"a":2,"text":"\ud83d\ude00\u0000","unpaired":"\ud800"},"big":1e9999)") +
      "]");
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  EXPECT_EQ(Rows->size(), 1u);
  // Beyond the SIMD parser's depth domain, the legacy reader remains valid.
  auto Deep = parsePatchRows("[" +
                             row("1", ",\"extra\":" + std::string(1030, '[') +
                                          "0" + std::string(1030, ']')) +
                             "]");
  ASSERT_TRUE(bool(Deep)) << llvm::toString(Deep.takeError());
  EXPECT_EQ(Deep->size(), 1u);
  for (llvm::StringRef Extra :
       {R"(,"extra":[1,])", R"(,"extra":"\uZZZZ")", ",\"extra\":\"\xff\""})
    expectRejected("[" + row("1", Extra) + "]");
  expectRejected("[" + row() + ",]");
  expectRejected("[" + row() + "] false");
  expectRejected({});
}

TEST(PatchJSON, EmptyInputRetainsLegacySyntaxDiagnostics) {
  for (llvm::StringRef Text :
       {llvm::StringRef(), llvm::StringRef(""), llvm::StringRef(" \n\t")}) {
    auto Legacy = llvm::json::parse(Text);
    ASSERT_FALSE(bool(Legacy));
    const auto Diagnostic = llvm::toString(Legacy.takeError());
    auto Rows = parsePatchRows(Text);
    ASSERT_FALSE(bool(Rows));
    EXPECT_EQ(llvm::toString(Rows.takeError()), Diagnostic);
  }
}

TEST(PatchJSON, EnforcesRowLimitBeforeReturningPartialState) {
  const std::string Row = row();
  std::string Text = "[";
  for (unsigned I = 0; I < 65536; ++I) {
    if (I)
      Text += ',';
    Text += Row;
  }
  Text += ']';
  auto Rows = parsePatchRows(Text);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  EXPECT_EQ(Rows->size(), 65536u);
  Text.pop_back();
  Text += ',' + Row + ']';
  expectRejected(Text, "patches.json must be a bounded array");
}

TEST(PatchJSON, EnforcesTotalByteBudgetAndInputSizeBeforePublication) {
  std::string Text = "[{\"file_off\":\"0x0\",\"va\":\"0x1000\",\"kind\":"
                     "\"bytes\",\"size\":4194304,"
                     "\"original\":\"" +
                     std::string(8388608, 'c') + "\",\"current\":\"" +
                     std::string(8388608, '9') + "\"}]";
  auto Rows = parsePatchRows(Text);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  ASSERT_EQ(Rows->size(), 1u);
  EXPECT_EQ(Rows->front().size(), 4194304u);
  EXPECT_EQ(Rows->front().Current.back(), 0x99u);
  Text.pop_back();
  Text += ',' + row() + ']';
  expectRejected(Text, "invalid patch row fields");
  expectRejected(std::string((32u << 20) + 1, ' '),
                 "patches.json exceeds its size limit");
}
} // namespace
