//===- COFFFH4EncodingTests.cpp - Canonical C++ EH4 wire tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFFH4Encoding.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace neverd::coff_fh4;

struct EncodingCase {
  uint32_t Value;
  std::vector<uint8_t> Bytes;
};

TEST(COFFFH4Encoding, EncodesAndDecodesEveryCanonicalWidthBoundary) {
  const std::vector<EncodingCase> Cases = {
      {0, {0x00}},
      {0x7f, {0xfe}},
      {0x80, {0x01, 0x02}},
      {0x3fff, {0xfd, 0xff}},
      {0x4000, {0x03, 0x00, 0x02}},
      {0x1fffff, {0xfb, 0xff, 0xff}},
      {0x200000, {0x07, 0x00, 0x00, 0x02}},
      {0x0fffffff, {0xf7, 0xff, 0xff, 0xff}},
      {0x10000000, {0x0f, 0x00, 0x00, 0x00, 0x10}},
      {std::numeric_limits<uint32_t>::max(), {0x0f, 0xff, 0xff, 0xff, 0xff}},
  };

  for (const EncodingCase &TestCase : Cases) {
    SCOPED_TRACE(TestCase.Value);
    const auto Encoded = encodeCompressedUInt(TestCase.Value);
    EXPECT_EQ(Encoded.size(), TestCase.Bytes.size());
    EXPECT_TRUE(std::equal(Encoded.begin(), Encoded.end(),
                           TestCase.Bytes.begin(), TestCase.Bytes.end()));

    std::vector<uint8_t> WithTrailingBytes = TestCase.Bytes;
    WithTrailingBytes.push_back(0xa5);
    llvm::Expected<CompressedUInt> Decoded =
        decodeCompressedUInt(WithTrailingBytes);
    ASSERT_TRUE(static_cast<bool>(Decoded))
        << llvm::toString(Decoded.takeError());
    EXPECT_EQ(Decoded->Value, TestCase.Value);
    EXPECT_EQ(Decoded->Size, TestCase.Bytes.size());
  }
}

TEST(COFFFH4Encoding, RejectsTruncatedReservedAndOverlongSpellings) {
  const std::vector<std::vector<uint8_t>> Invalid = {
      {},
      {0x01},
      {0x03, 0x00},
      {0x07, 0x00, 0x00},
      {0x0f, 0x00, 0x00, 0x00},
      {0x05, 0x00},
      {0x03, 0x00, 0x00},
      {0x07, 0x00, 0x00, 0x00},
      {0x0f, 0xff, 0xff, 0xff, 0x0f},
      {0x1f, 0x00, 0x00, 0x00, 0x10},
  };

  for (const std::vector<uint8_t> &Bytes : Invalid) {
    llvm::Expected<CompressedUInt> Decoded = decodeCompressedUInt(Bytes);
    EXPECT_FALSE(static_cast<bool>(Decoded));
    if (!Decoded)
      EXPECT_FALSE(llvm::toString(Decoded.takeError()).empty());
  }
}

class TestImage {
public:
  explicit TestImage(size_t Size = 0x800) : Bytes(Size) {}

  void place(uint32_t RVA, std::initializer_list<uint8_t> Data) {
    ASSERT_LE(uint64_t(RVA) + Data.size(), Bytes.size());
    std::copy(Data.begin(), Data.end(), Bytes.begin() + RVA);
  }

  void write32(uint32_t RVA, uint32_t Value) {
    ASSERT_LE(uint64_t(RVA) + 4, Bytes.size());
    for (unsigned I = 0; I != 4; ++I)
      Bytes[RVA + I] = static_cast<uint8_t>(Value >> (I * 8));
  }

  llvm::Expected<llvm::ArrayRef<uint8_t>> read(uint32_t RVA,
                                               uint32_t Size) const {
    if (uint64_t(RVA) + Size > Bytes.size())
      return llvm::createStringError(llvm::errc::bad_address,
                                     "test RVA is not mapped");
    return llvm::ArrayRef<uint8_t>(Bytes).slice(RVA, Size);
  }

  std::vector<uint8_t> Bytes;
};

TestImage makeMinimalCatchAllImage() {
  TestImage Image;
  Image.place(0x100, {0x38});
  Image.write32(0x101, 0x200);
  Image.write32(0x105, 0x300);
  Image.write32(0x109, 0x500);

  // Two empty unwind states, both pointing at the encoded empty-state byte.
  Image.place(0x200, {0x04, 0x08, 0x10});

  // One try row and one catch-all handler row.
  Image.place(0x300, {0x02, 0x00, 0x00, 0x02});
  Image.write32(0x304, 0x400);
  Image.place(0x400, {0x02, 0x01, 0x80});
  Image.write32(0x403, 0x2000);

  // Three IP states: empty, state zero, empty.
  Image.place(0x500, {0x06, 0x00, 0x00, 0x20, 0x02, 0x20, 0x00});
  return Image;
}

std::string takeExpectedError(llvm::Expected<FuncInfoLayout> &Parsed) {
  EXPECT_FALSE(static_cast<bool>(Parsed));
  if (Parsed)
    return {};
  return llvm::toString(Parsed.takeError());
}

TEST(COFFFH4Encoding, ParsesMinimalCatchAllWithExactPhysicalRanges) {
  const TestImage Image = makeMinimalCatchAllImage();
  auto Parsed = parseFuncInfoLayout(0x100, [&](uint32_t RVA, uint32_t Size) {
    return Image.read(RVA, Size);
  });
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());

  EXPECT_EQ(Parsed->Header, 0x38);
  EXPECT_EQ(Parsed->HeaderRange, (ByteRange{0x100, 0x10d}));
  ASSERT_TRUE(Parsed->Unwind.has_value());
  EXPECT_EQ(Parsed->Unwind->Range, (ByteRange{0x200, 0x203}));
  ASSERT_EQ(Parsed->Unwind->Entries.size(), 2u);
  EXPECT_EQ(Parsed->Unwind->Entries[0].Range, (ByteRange{0x201, 0x202}));
  EXPECT_EQ(Parsed->Unwind->Entries[1].Range, (ByteRange{0x202, 0x203}));
  EXPECT_EQ(Parsed->Unwind->Entries[0].ToState, -1);
  EXPECT_EQ(Parsed->Unwind->Entries[1].ToState, -1);

  ASSERT_TRUE(Parsed->Try.has_value());
  EXPECT_EQ(Parsed->Try->Range, (ByteRange{0x300, 0x308}));
  ASSERT_EQ(Parsed->Try->Entries.size(), 1u);
  const TryEntry &Try = Parsed->Try->Entries.front();
  EXPECT_EQ(Try.Range, (ByteRange{0x301, 0x308}));
  EXPECT_EQ(Try.Handlers.Range, (ByteRange{0x400, 0x407}));
  ASSERT_EQ(Try.Handlers.Entries.size(), 1u);
  const HandlerEntry &Catch = Try.Handlers.Entries.front();
  EXPECT_EQ(Catch.Range, (ByteRange{0x401, 0x407}));
  EXPECT_EQ(Catch.Adjectives, 0x40u);
  EXPECT_EQ(Catch.HandlerRVA, 0x2000u);

  ASSERT_TRUE(Parsed->States.has_value());
  EXPECT_EQ(Parsed->States->Range, (ByteRange{0x500, 0x507}));
  ASSERT_EQ(Parsed->States->Entries.size(), 3u);
  EXPECT_EQ(Parsed->States->Entries[0].Range, (ByteRange{0x501, 0x503}));
  EXPECT_EQ(Parsed->States->Entries[1].Range, (ByteRange{0x503, 0x505}));
  EXPECT_EQ(Parsed->States->Entries[2].Range, (ByteRange{0x505, 0x507}));
  EXPECT_EQ(Parsed->States->Entries[0].FunctionOffset, 0u);
  EXPECT_EQ(Parsed->States->Entries[1].FunctionOffset, 0x10u);
  EXPECT_EQ(Parsed->States->Entries[2].FunctionOffset, 0x20u);
}

TEST(COFFFH4Encoding, ParsesTypedHandlerWithCanonicalVariableWidth) {
  TestImage Image = makeMinimalCatchAllImage();
  Image.place(0x400, {0x02, 0x03, 0x12});
  Image.write32(0x403, 0x1800);
  Image.write32(0x407, 0x2000);

  auto Parsed = parseFuncInfoLayout(0x100, [&](uint32_t RVA, uint32_t Size) {
    return Image.read(RVA, Size);
  });
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->Try.has_value());
  ASSERT_EQ(Parsed->Try->Entries.size(), 1u);
  const HandlerEntry &Catch =
      Parsed->Try->Entries.front().Handlers.Entries.front();
  EXPECT_EQ(Catch.Range, (ByteRange{0x401, 0x40b}));
  EXPECT_EQ(Catch.Header, 0x03);
  EXPECT_EQ(Catch.Adjectives, 9u);
  EXPECT_EQ(Catch.TypeDescriptorRVA, 0x1800u);
  EXPECT_EQ(Catch.HandlerRVA, 0x2000u);
}

TEST(COFFFH4Encoding, RejectsNonCanonicalRowsAndInteriorPredecessors) {
  {
    TestImage Image = makeMinimalCatchAllImage();
    Image.place(0x500, {0x01, 0x00});
    auto Parsed = parseFuncInfoLayout(0x100, [&](uint32_t RVA, uint32_t Size) {
      return Image.read(RVA, Size);
    });
    EXPECT_NE(takeExpectedError(Parsed).find("overlong"), std::string::npos);
  }

  {
    TestImage Image = makeMinimalCatchAllImage();
    Image.place(0x200, {0x04, 0x06, 0x00, 0x09, 0x00, 0x00, 0x10});
    auto Parsed = parseFuncInfoLayout(0x100, [&](uint32_t RVA, uint32_t Size) {
      return Image.read(RVA, Size);
    });
    EXPECT_NE(takeExpectedError(Parsed).find("entry boundary"),
              std::string::npos);
  }
}

TEST(COFFFH4Encoding, EnforcesAggregateBudgetsAndMappedBytes) {
  const TestImage Image = makeMinimalCatchAllImage();
  auto TooManyRecords = parseFuncInfoLayout(
      0x100, [&](uint32_t RVA, uint32_t Size) { return Image.read(RVA, Size); },
      ParseLimits{/*MaxRecords=*/1, /*MaxBytes=*/1u << 20});
  EXPECT_NE(takeExpectedError(TooManyRecords).find("record budget"),
            std::string::npos);

  auto TooManyBytes = parseFuncInfoLayout(
      0x100, [&](uint32_t RVA, uint32_t Size) { return Image.read(RVA, Size); },
      ParseLimits{/*MaxRecords=*/1u << 16, /*MaxBytes=*/12});
  EXPECT_NE(takeExpectedError(TooManyBytes).find("byte budget"),
            std::string::npos);

  auto Truncated = parseFuncInfoLayout(
      0x100,
      [&](uint32_t RVA,
          uint32_t Size) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
        if (RVA >= 0x501 && RVA < 0x507)
          return llvm::createStringError(llvm::errc::bad_address,
                                         "truncated IP table");
        return Image.read(RVA, Size);
      });
  EXPECT_FALSE(takeExpectedError(Truncated).empty());
}

TEST(COFFFH4Encoding, RejectsReservedHandlerHeadersAndOverlappingTables) {
  {
    TestImage Image = makeMinimalCatchAllImage();
    Image.Bytes[0x401] = 0xc0;
    auto Parsed = parseFuncInfoLayout(0x100, [&](uint32_t RVA, uint32_t Size) {
      return Image.read(RVA, Size);
    });
    EXPECT_NE(takeExpectedError(Parsed).find("handler header"),
              std::string::npos);
  }

  {
    TestImage Image(0x400);
    Image.place(0, {0x3c, 0x00});
    Image.write32(2, 0x100);
    Image.write32(6, 0x200);
    Image.write32(10, 0x0a);
    Image.place(0x100, {0x00});
    Image.place(0x200, {0x00});
    auto Parsed = parseFuncInfoLayout(
        0, [&](uint32_t RVA, uint32_t Size) { return Image.read(RVA, Size); });
    EXPECT_NE(takeExpectedError(Parsed).find("overlap"), std::string::npos);
  }
}

} // namespace
