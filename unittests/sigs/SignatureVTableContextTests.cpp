//===- SignatureVTableContextTests.cpp - Source-backed table contexts
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Primary RTTI, source slots, complete bodies and frozen candidate evidence.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RTTI.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureDB.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sigs;

namespace {

const std::string Table = "................................ 00 0000 0010 "
                          "!vtable :0000 ??_7Widget@@6B@ "
                          "^0000 destroy ^0008 action\n";

void put(BinaryImage &Img, uint64_t VA, uint64_t Value, unsigned Width = 4) {
  for (auto &Seg : Img.Segments)
    if (VA >= Seg.VA && VA - Seg.VA + Width <= Seg.Data.size()) {
      for (unsigned I = 0; I < Width; ++I)
        Seg.Data[VA - Seg.VA + I] = Value >> (8 * I);
      return;
    }
  ADD_FAILURE() << "fixture address is not backed";
}

BinaryImage tableImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x1000;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 0x300;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size, 0xcc);
  for (size_t Offset : {size_t(0), size_t(0x100)}) {
    std::fill_n(Code.Data.begin() + Offset, 32, 0x99);
    Code.Data[Offset] = Offset ? 0x56 : 0x55;
    Code.Data[Offset + 31] = 0xc3;
  }
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x5000;
  Data.Size = Data.FileSz = 0x400;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size, 0);
  Img.Segments.push_back(std::move(Data));
  put(Img, 0x5000, 0x5100, 8);
  put(Img, 0x5008, 0x1000, 8);
  put(Img, 0x5010, 0x1100, 8);
  Img.DataPtrRelocSlots.insert(0x5000);
  Img.CodePtrRelocSlots.insert(0x5008);
  Img.CodePtrRelocSlots.insert(0x5010);
  put(Img, 0x5100, 1);      // COL signature, primary offsets remain zero.
  put(Img, 0x510c, 0x4200); // Type RVA.
  put(Img, 0x5110, 0x4140); // Hierarchy RVA.
  put(Img, 0x5114, 0x4100); // Self RVA.
  put(Img, 0x5148, 1);      // One complete type in the hierarchy.
  put(Img, 0x514c, 0x4180);
  put(Img, 0x5180, 0x41a0);
  put(Img, 0x51a0, 0x4200);
  put(Img, 0x51ac, 0xffffffff);
  put(Img, 0x51b4, 0x40);
  put(Img, 0x51b8, 0x4140);
  const std::string Type = ".?AVWidget@@";
  std::copy(Type.begin(), Type.end(), Img.Segments[1].Data.begin() + 0x210);
  return Img;
}

std::string body(std::string Name, std::string Prefix = "55",
                 std::string Context = "!caller ") {
  return Prefix + std::string(60, '9') + "C3 00 0000 0020 " + Context +
         ":0000 " + Name + "\n";
}

void add(SignatureDB &DB, const std::string &Text) {
  auto Error = DB.loadPatternText(Text, "table-context");
  ASSERT_FALSE(Error) << llvm::toString(std::move(Error));
}

} // namespace

TEST(SignatureVTableContext, CompleteMetadataAndBodiesProduceCandidatesOnly) {
  SignatureDB DB;
  add(DB, Table + body("destroy") + body("action", "56"));
  auto Img = tableImage();
  ASSERT_EQ(coff_loader::readPrimaryVTableTypeName(Img, 0x5008), "Widget@@");
  DB.apply(Img, {0x1000, 0x1100});
  const auto Names = DB.buildCandidateNameMap();
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names.at(0x1000), "destroy");
  EXPECT_EQ(Names.at(0x1100), "action");
  EXPECT_TRUE(DB.buildNameMap().empty());
  EXPECT_TRUE(DB.findMatch(0x1000)->CallerConfirmed);
}

TEST(SignatureVTableContext, MetadataNeverActsAsFunctionBytes) {
  SignatureDB DB;
  add(DB, Table);
  DB.apply(tableImage(), {0x1000, 0x1100, 0x5008});
  EXPECT_TRUE(DB.matches().empty());
}

TEST(SignatureVTableContext, KnownSiblingContradictionRejectsTheWholeTable) {
  SignatureDB DB;
  add(DB, Table + body("destroy") + body("foreign", "56", ""));
  DB.apply(tableImage(), {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "foreign");
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1000));
}

TEST(SignatureVTableContext, WeakAlternativesAreNotUnconditionalAliases) {
  const std::string Alternatives =
      "................................ 00 0000 0010 !vtable "
      ":0000 ??_7Widget@@6B@ ^0000 destroy ^0000 fallback ^0008 action\n";
  for (bool SameBytes : {false, true}) {
    SignatureDB DB;
    add(DB, Alternatives + body("destroy") +
                body("fallback", SameBytes ? "55" : "57"));
    DB.apply(tableImage(), {0x1000, 0x1100});
    EXPECT_EQ(DB.buildCandidateNameMap().contains(0x1000), !SameBytes);
  }
}

TEST(SignatureVTableContext, ConflictingMatchingTableSourcesRemainUnknown) {
  SignatureDB DB;
  std::string Rival = Table;
  Rival.replace(Rival.find("destroy"), 7, "other");
  add(DB, Table + Rival + body("destroy") + body("other"));
  DB.apply(tableImage(), {0x1000, 0x1100});
  EXPECT_TRUE(DB.matches().empty());
}

TEST(SignatureVTableContext, WeakPartialAndRequiredBodiesCannotBeRequested) {
  for (const auto &Claim :
       {"55 00 0000 0020 !caller :0000 destroy\n",
        "..............................................................C3 "
        "00 0000 0020 !caller :0000 destroy\n",
        "55999999999999999999999999999999999999999999999999999999999999C3 "
        "00 0000 0020 !refs :0000 destroy ^0001 missing\n"}) {
    SignatureDB DB;
    auto Error = DB.loadPatternText(Table + Claim, "table-context");
    if (std::string(Claim).starts_with("55 00")) {
      EXPECT_TRUE(bool(Error));
      if (Error)
        llvm::consumeError(std::move(Error));
      continue;
    }
    ASSERT_FALSE(Error) << llvm::toString(std::move(Error));
    DB.apply(tableImage(), {0x1000, 0x1100});
    EXPECT_TRUE(DB.matches().empty());
  }
}

TEST(SignatureVTableContext, StorageRelocationsAndPrimaryRTTIAreRequired) {
  for (unsigned Case = 0; Case < 13; ++Case) {
    auto Img = tableImage();
    switch (Case) {
    case 0:
      Img.DataPtrRelocSlots.clear();
      break;
    case 1:
      Img.CodePtrRelocSlots.erase(0x5010);
      break;
    case 2:
      Img.CodePtrRelocSlots.insert(0x5018);
      break;
    case 3:
      Img.Segments[1].Flags = Img.Segments[1].Flags | SegmentFlags::Writable;
      break;
    case 4:
      Img.Segments[1].Flags = Img.Segments[1].Flags | SegmentFlags::Executable;
      break;
    case 5:
      Img.Segments.push_back(Img.Segments[1]);
      break;
    case 6:
      Img.Segments[1].Size = Img.Segments[1].FileSz = 0x10;
      break;
    case 7:
      put(Img, 0x5104, 8);
      break;
    case 8:
      put(Img, 0x5108, 4);
      break;
    case 9:
      put(Img, 0x5114, 0x4108);
      break;
    case 10:
      Img.Format = BinaryFormat::ELF;
      break;
    case 11:
      Img.Arch = Arch::X86;
      break;
    case 12:
      Img.IsRelocatable = true;
      break;
    }
    SignatureDB DB;
    add(DB, Table + body("destroy"));
    DB.apply(Img, {0x1000, 0x1100});
    EXPECT_TRUE(DB.matches().empty()) << Case;
  }
}

TEST(SignatureVTableContext, EmptyFrozenEvidenceCannotConfirmANewCycle) {
  auto Img = tableImage();
  for (uint64_t Address : {uint64_t(0x1000), uint64_t(0x1100)}) {
    put(Img, Address + 25, 0xe8, 1);
    const uint64_t Target = Address == 0x1000 ? 0x1100 : 0x1000;
    put(Img, Address + 26, uint32_t(Target - (Address + 30)));
  }
  const std::string Prefix = std::string(48, '9');
  SignatureDB DB;
  add(DB, Table + "55" + Prefix +
              "E8........99C3 00 0000 0020 !caller "
              ":0000 destroy ^001A action\n" +
              "56" + Prefix +
              "E8........99C3 00 0000 0020 !caller "
              ":0000 action ^001A destroy\n");
  DB.apply(Img, {0x1000, 0x1100});
  ASSERT_EQ(DB.buildCandidateNameMap().size(), 2u);
  for (const SigMatch &Match : DB.matches()) {
    EXPECT_TRUE(Match.CallerConfirmed);
    EXPECT_FALSE(Match.Confirmed);
  }
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureVTableContext, OnlyRequestedEntriesAndUnsortedInputAreSupported) {
  SignatureDB DB;
  add(DB, Table + body("destroy") + body("action", "56"));
  const auto Img = tableImage();
  DB.apply(Img, {0x1100});
  ASSERT_EQ(DB.buildCandidateNameMap().size(), 1u);
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "action");
  DB.apply(Img, {0x1100, 0x1000});
  EXPECT_EQ(DB.buildCandidateNameMap().size(), 2u);
}

TEST(SignatureVTableContext, SectionOwnershipCannotBeOverlappingOrWritable) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    auto Img = tableImage();
    Section Sec;
    Sec.VA = 0x5000;
    Sec.Size = 0x400;
    Sec.Flags = SegmentFlags::Readable;
    Img.Sections.push_back(Sec);
    if (Case == 1)
      Img.Sections.push_back(Sec);
    else if (Case == 2)
      Img.Sections.front().Size = 16;
    else if (Case == 3)
      Img.Sections.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    SignatureDB DB;
    add(DB, Table + body("destroy"));
    DB.apply(Img, {0x1000, 0x1100});
    EXPECT_EQ(DB.buildCandidateNameMap().contains(0x1000), Case == 0) << Case;
  }
}

TEST(SignatureVTableContext, EitherRelocationIndexProducesTheSameCandidates) {
  for (bool MoreData : {false, true}) {
    auto Img = tableImage();
    if (MoreData)
      for (uint64_t Address : {uint64_t(0x5300), uint64_t(0x5308), InvalidVA})
        Img.DataPtrRelocSlots.insert(Address);
    SignatureDB DB;
    add(DB, Table + body("destroy") + body("action", "56"));
    DB.apply(Img, {0x1000, 0x1100});
    ASSERT_EQ(DB.buildCandidateNameMap().size(), 2u);
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1000), "destroy");
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "action");
  }
}
