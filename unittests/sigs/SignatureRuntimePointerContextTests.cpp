//===- SignatureRuntimePointerContextTests.cpp - Runtime slot context -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent slot roles, complete operand coverage and conflicting evidence.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureDB.h"

#include <cstdint>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::sigs;

namespace {

std::string config(std::string Default = "dispatch") {
  return std::string(0x140 * 2, '.') +
         " 00 0000 0140 !loadcfg :0000 _load_config_used &0078 cf ^0078 " +
         Default + " &0120 xfg ^0120 extended\n";
}
const std::string Bodies =
    "FFE0 00 0000 0002 !caller :0000 dispatch\n"
    "FF25........ 00 0000 0006 !caller :0000 extended &0002 cf\n";

void put(BinaryImage &Img, uint64_t VA, uint64_t Value, unsigned Width) {
  for (auto &Seg : Img.Segments)
    if (VA >= Seg.VA && VA - Seg.VA + Width <= Seg.Data.size()) {
      for (unsigned I = 0; I < Width; ++I)
        Seg.Data[VA - Seg.VA + I] = Value >> (I * 8);
      return;
    }
  ADD_FAILURE() << "unbacked fixture address";
}

BinaryImage image() {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 0x200;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size, 0xcc);
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x5000;
  Data.Size = Data.FileSz = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Img.Segments.push_back(std::move(Data));
  put(Img, 0x1000, 0xe0ff, 2);
  put(Img, 0x1100, 0x25ff, 2);
  put(Img, 0x1102, 0x5000 - 0x1106, 4);
  put(Img, 0x5000, 0x1000, 8);
  put(Img, 0x5008, 0x1100, 8);
  Img.CodePtrRelocSlots.insert(0x5000);
  Img.CodePtrRelocSlots.insert(0x5008);
  Img.RuntimeCallablePointerSlots = {
      {0x5000, RuntimeCallablePointerSlotKind::GuardCFDispatch},
      {0x5008, RuntimeCallablePointerSlotKind::GuardXFGDispatch}};
  return Img;
}

void add(SignatureDB &DB, const std::string &Text) {
  auto Error = DB.loadPatternText(Text, "runtime-pointer-context");
  ASSERT_FALSE(Error) << llvm::toString(std::move(Error));
}

} // namespace

TEST(SignatureRuntimePointerContext, CompleteBindingsProduceCandidatesOnly) {
  SignatureDB DB;
  add(DB, config() + Bodies);
  DB.apply(image(), {0x1100, 0x1000}); // Entry order is not part of evidence.
  const auto Names = DB.buildCandidateNameMap();
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names.at(0x1000), "dispatch");
  EXPECT_EQ(Names.at(0x1100), "extended");
  EXPECT_TRUE(DB.buildNameMap().empty());
  EXPECT_TRUE(DB.findMatch(0x1100)->Confirmed);
}

TEST(SignatureRuntimePointerContext, RolesAloneAndUnboundWildcardsCannotName) {
  for (const auto &Text : {config(), Bodies}) {
    SignatureDB DB;
    add(DB, Text);
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_TRUE(DB.matches().empty());
  }
  for (const auto &Body :
       {"FF25........ 00 0000 0006 !caller :0000 extended\n",
        "FF25........ 00 0000 0006 !caller :0000 extended &0002 unknown\n",
        "FF25........ 00 0000 0006 !caller :0000 extended ^0002 cf\n",
        "FF25........ 00 0000 0006 !caller :0000 extended &0002 xfg\n"}) {
    SignatureDB DB;
    add(DB, config() + Body);
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_TRUE(DB.matches().empty()) << Body;
  }
}

TEST(SignatureRuntimePointerContext, WrongOperandCannotBorrowAnotherSlot) {
  SignatureDB DB;
  add(DB, config() + Bodies);
  auto Img = image();
  put(Img, 0x1102, 0x5008 - 0x1106, 4);
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().size(), 1u);
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureRuntimePointerContext, RequestedEntriesAndFullBodyAreRequired) {
  SignatureDB DB;
  add(DB, config() + Bodies);
  DB.apply(image(), {0x1000});
  EXPECT_EQ(DB.buildCandidateNameMap().size(), 1u);
  auto Img = image();
  put(Img, 0x1000, 0xc3, 1);
  put(Img, 0x1100, 0xc3, 1);
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_TRUE(DB.matches().empty());
}

TEST(SignatureRuntimePointerContext,
     InvalidStorageAndRoleConflictsStayUnknown) {
  for (unsigned Case = 0; Case < 13; ++Case) {
    auto Img = image();
    switch (Case) {
    case 0:
      Img.Format = BinaryFormat::ELF;
      break;
    case 1:
      Img.Bits = Bitness::Bits32;
      break;
    case 2:
      Img.IsRelocatable = true;
      break;
    case 3:
      Img.Segments[1].Flags = Img.Segments[1].Flags | SegmentFlags::Writable;
      break;
    case 4:
      Img.Segments[1].Flags = Img.Segments[1].Flags | SegmentFlags::Executable;
      break;
    case 5:
      Img.CodePtrRelocSlots.erase(0x5000);
      break;
    case 6:
      Img.RuntimeCallablePointerSlots.erase(
          Img.RuntimeCallablePointerSlots.begin());
      break;
    case 7:
      Img.Segments.push_back(Img.Segments[1]);
      break;
    case 8:
      Img.Segments[1].Data.resize(4);
      break;
    case 9:
      Img.RuntimeCallablePointerSlots.push_back(
          {0x5008, RuntimeCallablePointerSlotKind::GuardCFDispatch});
      break;
    case 10:
      Img.RuntimeCallablePointerSlots[1].SlotVA = 0x5000;
      break;
    case 11:
      put(Img, 0x5000, 0x5010, 8);
      break;
    case 12:
      Img.RuntimeCallablePointerSlots[0].SlotVA = 0x5001;
      break;
    }
    SignatureDB DB;
    add(DB, config() + Bodies);
    DB.apply(Img, {0x1000, 0x1100});
    EXPECT_TRUE(DB.matches().empty()) << Case;
  }
}

TEST(SignatureRuntimePointerContext, ConflictingSourceAndKnownDefaultsVeto) {
  SignatureDB DB;
  add(DB, config() + config("different") + Bodies);
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_TRUE(DB.matches().empty());
  DB.clear();
  add(DB, config() + Bodies);
  auto Img = image();
  Symbol Sym;
  Sym.Addr = 0x1000;
  Sym.IsFunc = true;
  Sym.Name = "different";
  Sym.Origin = NameOrigin::Stated;
  Img.Symbols.push_back(Sym);
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_TRUE(DB.matches().empty());
}

TEST(SignatureRuntimePointerContext, ParserRejectsIncompleteOrUnknownFields) {
  for (auto [Old, New] : {std::pair("&0078 cf", "&0078 cf &0078 repeated"),
                          {"^0078 dispatch", ""},
                          {"0078", "0070"},
                          {"&0078", "&0008"},
                          {"_load_config_used", "made_up"},
                          {"00 0000", "01 0000"},
                          {"0140 !loadcfg", "0120 !loadcfg"}}) {
    std::string Text = config();
    Text.replace(Text.find(Old), std::string(Old).size(), New);
    auto Parsed = PatternParser::parseLine(Text);
    EXPECT_FALSE(bool(Parsed)) << Old << " -> " << New;
    if (!Parsed)
      llvm::consumeError(Parsed.takeError());
  }
  const auto Fields = coff_loader::loadConfigCallableFields(Bitness::Bits64);
  ASSERT_FALSE(Fields.empty());
  EXPECT_EQ(Fields.front().Offset, 0x70u);
}
