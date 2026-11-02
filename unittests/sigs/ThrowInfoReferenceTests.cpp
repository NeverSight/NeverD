//===- ThrowInfoReferenceTests.cpp - Exception type reference checks ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete metadata ownership, unsupported graphs and candidate-only naming.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RTTI.h"
#include "neverd/loader/Limits.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::coff_loader;
using namespace neverd::sigs;

namespace {

BinaryImage makeThrowImage() {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Base = 0x1000;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 32;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(32, 0x90);
  Code.Data[4] = 0x48;
  Code.Data[5] = 0x8d;
  Code.Data[6] = 0x15;
  writeLE<uint32_t>(Code.Data.data() + 7, 0x2100 - 0x100b);
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 0x400;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(0x400);
  auto Put = [&](size_t Offset, uint32_t Value) {
    writeLE<uint32_t>(Data.Data.data() + Offset, Value);
  };
  Put(0x104, 0x10);   // Cleanup code RVA.
  Put(0x10c, 0x1140); // Catchable array RVA.
  Put(0x140, 2);
  Put(0x144, 0x1180);
  Put(0x148, 0x11a0);
  for (unsigned I = 0; I < 2; ++I) {
    const size_t At = 0x180 + I * 0x20;
    Put(At + 4, 0x1200 + I * 0x40);
    Put(At + 8, I ? 0x10 : 0);
    Put(At + 12, 0xffffffff);
    Put(At + 20, I ? 0x10 : 0x20);
    Put(At + 24, 0x10);
    writeLE<uint64_t>(Data.Data.data() + 0x200 + I * 0x40, 0x2300);
  }
  constexpr char Derived[] = ".?AVWidget@@";
  constexpr char Base[] = ".?AVBase@@";
  std::memcpy(Data.Data.data() + 0x210, Derived, sizeof(Derived));
  std::memcpy(Data.Data.data() + 0x250, Base, sizeof(Base));
  writeLE<uint64_t>(Data.Data.data() + 0x300, 0x1000);
  Img.Segments.push_back(std::move(Data));
  return Img;
}

std::string callerLine(const BinaryImage &Img, llvm::StringRef Name,
                       llvm::StringRef Reference) {
  constexpr char Hex[] = "0123456789ABCDEF";
  std::string Text;
  for (size_t I = 0; I < 32; ++I) {
    const uint8_t Byte = Img.Segments.front().Data[I];
    if (I >= 7 && I < 11)
      Text += "..";
    else {
      Text += Hex[Byte >> 4];
      Text += Hex[Byte & 15];
    }
  }
  return Text + " 00 0000 0020 !refs :0000 " + Name.str() + " ^0007 " +
         Reference.str() + "\n";
}

} // namespace

TEST(ThrowInfoReference, ExactNamesRetainQualifiersAndCatchableCounts) {
  for (auto Name : {"_TI2?AVWidget@@", "_TICVU1?AUStruct@@"}) {
    const auto Type = throwInfoTypeName(Name);
    ASSERT_TRUE(Type) << Name;
  }
  EXPECT_EQ(throwInfoTypeName("_TICVU1?AUStruct@@")->Qualifiers, 7u);
  for (auto Name : {"_TI0?AVWidget@@", "_TI02?AVWidget@@", "_TIVC2?AVWidget@@",
                    "_TI999999999999999999?AVWidget@@", "_TI1H", "_TI2?AV@@",
                    "_TI2?AVWith Space@@", "_TI2?AVWidget@"})
    EXPECT_FALSE(throwInfoTypeName(Name)) << Name;
  EXPECT_FALSE(throwInfoTypeName(
      "_TI" +
      std::to_string(limits::loader::kMsvcThrowInfoMaxCatchableTypes + 1) +
      "?AVWidget@@"));
}

TEST(ThrowInfoReference, CompleteGraphSelectsACandidateWithoutIdentity) {
  for (bool Reverse : {false, true}) {
    auto Img = makeThrowImage();
    const auto Type = readThrowInfoType(Img, 0x2100);
    ASSERT_TRUE(Type);
    EXPECT_EQ(*Type, *throwInfoTypeName("_TI2?AVWidget@@"));
    auto First = callerLine(Img, "throws_widget", "_TI2?AVWidget@@");
    auto Second = callerLine(Img, "throws_other", "_TI2?AVOther@@");
    SignatureDB DB;
    ASSERT_FALSE(DB.loadPatternText(Reverse ? Second + First : First + Second,
                                    "exceptions"));
    DB.apply(Img, {0x1000});
    ASSERT_TRUE(DB.buildCandidateNameMap().contains(0x1000));
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1000), "throws_widget");
    EXPECT_TRUE(DB.buildNameMap().empty());
    constexpr char Other[] = ".?AVOther@@";
    std::memcpy(Img.Segments.back().Data.data() + 0x210, Other, sizeof(Other));
    DB.apply(Img, {0x1000});
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1000), "throws_other");
    EXPECT_TRUE(DB.buildNameMap().empty());
    writeLE<uint32_t>(Img.Segments.back().Data.data() + 0x140, 0);
    DB.apply(Img, {0x1000});
    EXPECT_TRUE(DB.buildCandidateNameMap().empty());
  }
}

TEST(ThrowInfoReference, DescriptorCachesMayBeWritableButMetadataMustAgree) {
  auto Img = makeThrowImage();
  auto &Data = Img.Segments.back().Data;
  Segment Types;
  Types.VA = 0x3000;
  Types.Size = Types.FileSz = 0x80;
  Types.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Types.Data.assign(Data.begin() + 0x200, Data.begin() + 0x280);
  writeLE<uint32_t>(Data.data() + 0x184, 0x2000);
  writeLE<uint32_t>(Data.data() + 0x1a4, 0x2040);
  writeLE<uint32_t>(Data.data() + 0x100, 7);
  Img.Segments.push_back(std::move(Types));
  const auto Type = readThrowInfoType(Img, 0x2100);
  ASSERT_TRUE(Type);
  EXPECT_EQ(*Type, *throwInfoTypeName("_TICVU2?AVWidget@@"));
  for (auto Name :
       {"_TI2?AVWidget@@", "_TICVU1?AVWidget@@", "_TICVU2?AUWidget@@"}) {
    SignatureDB DB;
    ASSERT_FALSE(
        DB.loadPatternText(callerLine(Img, "wrong", Name), "mismatch"));
    DB.apply(Img, {0x1000});
    EXPECT_TRUE(DB.buildCandidateNameMap().empty()) << Name;
  }
}

TEST(ThrowInfoReference, EveryGraphRecordMustBeValidAndNonoverlapping) {
  const std::vector<std::pair<size_t, uint32_t>> Changes = {
      {0x100, 8},
      {0x104, 0x1100},
      {0x108, 1},
      {0x10c, 0},
      {0x10c, 0x13ff},
      {0x140, 0},
      {0x140, limits::loader::kMsvcThrowInfoMaxCatchableTypes + 1},
      {0x144, 0},
      {0x148, 0x1180},
      {0x148, 0x1190},
      {0x180, 1},
      {0x180, 4},
      {0x180, 0x10},
      {0x184, 0},
      {0x188, 1},
      {0x18c, 0},
      {0x190, 1},
      {0x194, 0},
      {0x198, 0x1100},
      {0x1a4, 0x1200},
      {0x1a8, 0x21},
      {0x1b4, 0x20},
      {0x1b4, 0x80000000},
      {0x200, 0},
      {0x208, 1},
      {0x210, 0},
      {0x240, 0x2310},
      {0x300, 0x2000}};
  for (auto [Offset, Value] : Changes) {
    auto Img = makeThrowImage();
    writeLE<uint32_t>(Img.Segments.back().Data.data() + Offset, Value);
    EXPECT_FALSE(readThrowInfoType(Img, 0x2100)) << Offset << ":" << Value;
    SignatureDB DB;
    ASSERT_FALSE(DB.loadPatternText(
        callerLine(Img, "throws_widget", "_TI2?AVWidget@@"), "bad"));
    DB.apply(Img, {0x1000});
    EXPECT_TRUE(DB.buildCandidateNameMap().empty()) << Offset;
  }
}

TEST(ThrowInfoReference, UnsupportedImagesStorageAndAddressFormsStayUnknown) {
  for (unsigned Variant = 0; Variant < 9; ++Variant) {
    auto Img = makeThrowImage();
    switch (Variant) {
    case 0:
      Img.Arch = Arch::X86;
      break;
    case 1:
      Img.Format = BinaryFormat::ELF;
      break;
    case 2:
      Img.Bits = Bitness::Bits32;
      break;
    case 3:
      Img.Segments.back().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 4:
      Img.Segments.back().Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 5:
      Img.Segments.back().Data.resize(0x259);
      break;
    case 6:
      Img.Segments.back().Size = 0x259;
      break;
    case 7:
      Img.Base = std::numeric_limits<uint64_t>::max() - 0x1000;
      break;
    case 8:
      Img.Segments.back().Data[0x254] = ' ';
      break;
    }
    EXPECT_FALSE(readThrowInfoType(Img, 0x2100)) << Variant;
  }
  for (uint8_t Prefix : {0x67, 0xf3}) {
    auto Img = makeThrowImage();
    Img.Segments.front().Data[3] = Prefix;
    SignatureDB DB;
    ASSERT_FALSE(DB.loadPatternText(
        callerLine(Img, "throws_widget", "_TI2?AVWidget@@"), "prefix"));
    DB.apply(Img, {0x1000});
    EXPECT_TRUE(DB.buildCandidateNameMap().empty());
  }
}
