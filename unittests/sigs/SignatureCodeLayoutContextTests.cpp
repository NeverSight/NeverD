//===- SignatureCodeLayoutContextTests.cpp - Source layout candidates ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent roots, complete source extents and frozen shared-data relations.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
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

const std::string RootBytes =
    "4883EC28FF15........4885C0488905........0F95C04883C428C3";
const std::string Root =
    RootBytes + " 00 0000 001C :0000 source_root ^0006 __imp_SourceAllocate\n";

std::string layout(std::string Left = "left", std::string Right = "right") {
  return "488B05........C3" + RootBytes +
         "488B05........C3 00 0000 002C "
         "!layout :0000 " +
         Left + " :0008 source_root :0024 " + Right +
         " &0003 state ^000E __imp_SourceAllocate &0018 state &0027 state\n";
}

void add(SignatureDB &DB, const std::string &Text) {
  auto Err = DB.loadPatternText(Text, "code-layout");
  ASSERT_FALSE(Err) << llvm::toString(std::move(Err));
}

void put(BinaryImage &Img, uint32_t At, uint32_t Value) {
  for (unsigned I = 0; I < 4; ++I)
    Img.Segments.front().Data[At + I] = Value >> (8 * I);
}

BinaryImage image() {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0x48, 0x8b, 0x05, 0,    0,    0,    0,    0xc3, 0x48,
               0x83, 0xec, 0x28, 0xff, 0x15, 0,    0,    0,    0,
               0x48, 0x85, 0xc0, 0x48, 0x89, 0x05, 0,    0,    0,
               0,    0x0f, 0x95, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3,
               0x48, 0x8b, 0x05, 0,    0,    0,    0,    0xc3};
  Code.Size = Code.FileSz = Code.Data.size();
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x5000;
  Data.Size = 0x80;
  Data.FileSz = 0x20;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.FileSz);
  Img.Segments.push_back(std::move(Data));
  put(Img, 3, 0x5060 - 0x1007);
  put(Img, 14, 0x5000 - 0x1012);
  put(Img, 24, 0x5060 - 0x101c);
  put(Img, 39, 0x5060 - 0x102b);
  Import Imp;
  Imp.Name = "SourceAllocate";
  Imp.IATAddr = 0x5000;
  Img.Imports.push_back(std::move(Imp));
  return Img;
}

} // namespace

TEST(SignatureCodeLayoutContext,
     FullSourceRelationAddsOnlyRequestedCandidates) {
  SignatureDB DB;
  add(DB, Root + layout());
  DB.apply(image(), {0x1024, 0x1008, 0x1000});
  const auto Names = DB.buildCandidateNameMap();
  ASSERT_EQ(Names.size(), 3u);
  EXPECT_EQ(Names.at(0x1000), "left");
  EXPECT_EQ(Names.at(0x1024), "right");
  EXPECT_EQ(DB.findMatch(0x1000)->FuncLen, 8u);
  EXPECT_EQ(DB.findMatch(0x1024)->FuncLen, 8u);
  EXPECT_TRUE(DB.findMatch(0x1000)->Confirmed);
  EXPECT_FALSE(DB.findMatch(0x1000)->CallerConfirmed);
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureCodeLayoutContext, CanonicalReadModifyWriteKeepsItsRel32Bias) {
  auto Img = image();
  auto &Code = Img.Segments.front();
  const std::vector<uint8_t> Clear{0x48, 0x83, 0x25, 0, 0,   0,
                                   0,    0,    0xb0, 1, 0xc3};
  Code.Data.resize(47);
  std::copy(Clear.begin(), Clear.end(), Code.Data.begin() + 36);
  Code.Size = Code.FileSz = Code.Data.size();
  put(Img, 39, 0x5060 - 0x102c);
  auto Text = layout();
  const std::string Last = "488B05........C3 00";
  Text.replace(Text.find(Last), Last.size(), "488325........00B001C3 00");
  Text.replace(Text.find("002C !layout"), 4, "002F");
  SignatureDB DB;
  add(DB, Root + Text);
  DB.apply(Img, {0x1000, 0x1008, 0x1024});
  ASSERT_TRUE(DB.findMatch(0x1024));
  EXPECT_EQ(DB.findMatch(0x1024)->FuncLen, 11u);
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1000), "left");
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureCodeLayoutContext,
     LayoutNeverEntersGlobalMatchingOrCreatesARoot) {
  SignatureDB DB;
  add(DB, layout());
  DB.apply(image(), {0x1000, 0x1008, 0x1024});
  EXPECT_TRUE(DB.matches().empty());
  add(DB, Root);
  DB.apply(image(), {0x1008, 0x1024});
  EXPECT_EQ(DB.matches().size(), 1u); // Missing member entry refuses the unit.
}

TEST(SignatureCodeLayoutContext, AmbiguousOrContextDependentRootCannotStart) {
  for (const std::string &RootText :
       {RootBytes + " 00 0000 001C !caller :0000 source_root\n",
        Root + RootBytes +
            " 00 0000 001C :0000 different_root ^0006 "
            "__imp_SourceAllocate\n"}) {
    SignatureDB DB;
    add(DB, RootText + layout());
    DB.apply(image(), {0x1000, 0x1008, 0x1024});
    EXPECT_FALSE(DB.findMatch(0x1000));
    EXPECT_FALSE(DB.findMatch(0x1024));
  }
}

TEST(SignatureCodeLayoutContext, ContradictingLayoutsRetainAmbiguity) {
  SignatureDB DB;
  add(DB, Root + layout() + layout("other_left", "other_right"));
  DB.apply(image(), {0x1000, 0x1008, 0x1024});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1000));
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1024));
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureCodeLayoutContext,
     IncomingSettlementDoesNotCertifyAnOrdinaryRoot) {
  auto Img = image();
  auto &Code = Img.Segments.front();
  Code.Data.resize(0x117, 0xcc);
  Code.Size = Code.FileSz = Code.Data.size();
  const std::vector<uint8_t> Parent = {
      0x55, 0x56, 0x57, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20, 0xe8, 0,
      0,    0,    0,    0x48, 0x83, 0xc4, 0x20, 0x5f, 0x5e, 0x5d, 0xc3};
  std::copy(Parent.begin(), Parent.end(), Code.Data.begin() + 0x100);
  put(Img, 0x10b, 0x1008 - 0x110f);
  SignatureDB DB;
  add(DB,
      Root + RootBytes +
          " 00 0000 001C :0000 rival_root ^0006 __imp_SourceAllocate\n" +
          RootBytes +
          " 00 0000 001C !caller :0000 source_root ^0006 __imp_SourceAllocate\n"
          "5556574889E54883EC20E8........4883C4205F5E5DC3 00 0000 0017 "
          ":0000 parent ^000B source_root\n" +
          layout());
  DB.apply(Img, {0x1000, 0x1008, 0x1024, 0x1100});
  ASSERT_TRUE(
      std::any_of(DB.matches().begin(), DB.matches().end(), [](const auto &M) {
        return M.Address == 0x1008 && M.CallerConfirmed;
      }));
  EXPECT_FALSE(DB.findMatch(0x1000));
  EXPECT_FALSE(DB.findMatch(0x1024));
}

TEST(SignatureCodeLayoutContext,
     StatedFunctionAndExportContradictionsAreRespected) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    auto Img = image();
    auto Sym = Symbol::makeFunc(0x1000);
    Sym.Name = "different_source";
    Sym.Origin = NameOrigin::Stated;
    if (Case == 0)
      Img.Symbols.push_back(Sym);
    if (Case == 1) {
      Img.Symbols.push_back(Sym);
      Sym.Name = "left";
      Img.Symbols.push_back(Sym);
    }
    if (Case == 2) {
      Sym.IsFunc = false;
      Img.Symbols.push_back(Sym);
    }
    if (Case == 3) {
      Export Exp;
      Exp.Name = "different_source";
      Exp.Addr = 0x1000;
      Img.Exports.push_back(std::move(Exp));
    }
    SignatureDB DB;
    add(DB, Root + layout());
    DB.apply(Img, {0x1000, 0x1008, 0x1024});
    EXPECT_EQ(bool(DB.findMatch(0x1024)), Case == 1 || Case == 2) << Case;
    if (Case == 1)
      EXPECT_EQ(DB.buildNameMap().at(0x1000), "left");
  }
}

TEST(SignatureCodeLayoutContext, ExistingContradictionRefusesTheWholeUnit) {
  SignatureDB DB;
  add(DB,
      Root + layout() + "488B05........C3 00 0000 0008 :0000 another_getter\n");
  DB.apply(image(), {0x1000, 0x1008, 0x1024});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1000), "another_getter");
  EXPECT_EQ(DB.matches().size(), 3u); // Original byte claims only.
}

TEST(SignatureCodeLayoutContext, OperandBytesOwnersAndImportsAreAllRequired) {
  for (unsigned Case = 0; Case < 13; ++Case) {
    auto Img = image();
    switch (Case) {
    case 0:
      put(Img, 39, 0x5068 - 0x102b);
      break;
    case 1:
      Img.Imports.front().Name = "DifferentExport";
      break;
    case 2:
      Img.Imports.push_back(Img.Imports.front());
      break;
    case 3:
      Img.Segments.front().Flags =
          Img.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 4:
      Img.Segments.front().Data[43] = 0xcc;
      break;
    case 5:
      Img.Segments.front().Data.pop_back();
      break;
    case 6:
      Img.Segments[1].Size = 0x64;
      break; // Truncated complete data load.
    case 7:
      Img.IsRelocatable = true;
      break;
    case 8:
      Img.Segments.push_back(Img.Segments.front());
      break;
    case 9:
      Img.Format = BinaryFormat::ELF;
      break;
    case 10:
      Img.Arch = Arch::X86;
      break;
    case 11:
      Img.Segments.front().Size = InvalidVA;
      break;
    case 12:
      Img.Segments[1].Flags = Img.Segments[1].Flags | SegmentFlags::Executable;
      break;
    }
    SignatureDB DB;
    add(DB, Root + layout());
    DB.apply(Img, {0x1000, 0x1008, 0x1024});
    EXPECT_FALSE(DB.findMatch(0x1000)) << Case;
    EXPECT_FALSE(DB.findMatch(0x1024)) << Case;
  }
}

TEST(SignatureCodeLayoutContext, ReloadAndReapplyCannotRetainAnOldBinding) {
  SignatureDB DB;
  add(DB, Root + layout());
  auto Img = image();
  DB.apply(Img, {0x1000, 0x1008, 0x1024});
  ASSERT_TRUE(DB.findMatch(0x1000));
  put(Img, 39, 0x5068 - 0x102b);
  DB.apply(Img, {0x1000, 0x1008, 0x1024});
  EXPECT_FALSE(DB.findMatch(0x1000));
  add(DB, Root);
  DB.apply(image(), {0x1000, 0x1008, 0x1024});
  EXPECT_FALSE(DB.findMatch(0x1000));
}

TEST(SignatureCodeLayoutContext, MalformedLayoutsCannotBypassCompleteEvidence) {
  for (unsigned Case = 0; Case < 8; ++Case) {
    auto Text = layout();
    auto Replace = [&](std::string From, std::string To) {
      Text.replace(Text.find(From), From.size(), To);
    };
    switch (Case) {
    case 0:
      Replace("&0027 state", "");
      break;
    case 1:
      Replace("&0027 state", "&0027 different_state");
      break;
    case 2:
      Replace("^000E", "&000E");
      break;
    case 3:
      Replace(":0024", ":0027");
      break;
    case 4:
      Replace("488B05", "488B..");
      break;
    case 5:
      Replace("&0003 state", "%0003 00000000:source_root");
      break;
    case 6:
      Replace(":0008 source_root", ":0008 left");
      break;
    case 7:
      Replace("002C !layout", "002D !layout");
      break;
    }
    auto Parsed = PatternParser::parseLine(Text);
    EXPECT_FALSE(bool(Parsed)) << Case;
    if (!Parsed)
      llvm::consumeError(Parsed.takeError());
  }
}
