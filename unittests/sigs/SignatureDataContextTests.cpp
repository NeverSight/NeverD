//===- SignatureDataContextTests.cpp - External-data context tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent data anchors, storage bounds and frozen candidate settlement.
///
//===----------------------------------------------------------------------===//

#include "../../lib/sigs/DataReference.h"
#include "../../lib/sigs/LocalDataReference.h"
#include "../../lib/sigs/X86/X86DataReference.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sigs;

namespace {

const std::string Body = "90909090909090909090909090909090488B05........C3";

std::string root(std::string Name = "anchor", bool Refine = false,
                 std::string Ref = " &0013 global", std::string Bytes = Body) {
  return Bytes + " 00 0000 0018 " + (Refine ? "!caller " : "") + ":0000 " +
         Name + (Refine ? Ref : "") + "\n";
}

std::string getter(std::string Name = "getter",
                   std::string Ref = "&0003 global") {
  return "488D05........C3 00 0000 0008 !refs :0000 " + Name + " " + Ref + "\n";
}

void displacement(BinaryImage &Img, uint64_t Field, uint64_t Next,
                  uint64_t Target) {
  const uint32_t Value = static_cast<uint32_t>(Target - Next);
  for (unsigned I = 0; I < 4; ++I)
    Img.Segments.front().Data[Field - 0x1000 + I] = Value >> (I * 8);
}

BinaryImage image() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 0x300;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size, 0xcc);
  std::fill_n(Code.Data.begin(), 16, 0x90);
  const uint8_t Load[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0, 0xc3};
  std::copy(std::begin(Load), std::end(Load), Code.Data.begin() + 16);
  std::copy(std::begin(Load), std::end(Load), Code.Data.begin() + 0x100);
  Code.Data[0x101] = 0x8d;
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data)); // Deliberately zero-fill storage.
  displacement(Img, 0x1013, 0x1017, 0x2020);
  displacement(Img, 0x1103, 0x1107, 0x2020);
  return Img;
}

void add(SignatureDB &DB, const std::string &Patterns) {
  auto Error = DB.loadPatternText(Patterns, "data-context");
  ASSERT_FALSE(Error) << llvm::toString(std::move(Error));
}

const std::string ParentBody =
    "91919191919191919191919191919191488B05........488D0D........C3";

std::string dataParent(bool Refine = false,
                       std::string Refs = " %0013 00000013:anchor ^001A child",
                       std::string Bytes = ParentBody) {
  return Bytes + " 00 0000 001F " + (Refine ? "!caller" : "!refs") +
         " :0000 parent" + (Refine ? Refs : " %0013 00000013:anchor") + "\n";
}

BinaryImage callerImage() {
  auto Img = image();
  auto &Code = Img.Segments.front().Data;
  std::fill_n(Code.begin() + 0x80, 16, 0x91);
  const uint8_t Operands[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0,   0x48,
                              0x8d, 0x0d, 0,    0, 0, 0, 0xc3};
  std::copy(std::begin(Operands), std::end(Operands), Code.begin() + 0x90);
  displacement(Img, 0x1093, 0x1097, 0x2020);
  displacement(Img, 0x109a, 0x109e, 0x1100);
  return Img;
}

const std::string Child = "488D05........C3 00 0000 0008 !caller :0000 child\n";

} // namespace

TEST(SignatureDataContext, CompleteIndependentAnchorSupportsCandidateOnly) {
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + getter() +
              getter("rival", "&0003 other"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
  ASSERT_NE(DB.findMatch(0x1100), nullptr);
  EXPECT_TRUE(DB.findMatch(0x1100)->Confirmed);
  EXPECT_TRUE(DB.buildNameMap().empty());
  DB.apply(image(), {0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, FrozenBindingsResolveDeferredCallerAlternatives) {
  const std::string Parent = ParentBody + " 00 0000 001F :0000 parent\n";
  const std::string Source = dataParent(true, " ^001A child ^001A rival");
  const std::string Children =
      "488D05........C3 00 0000 0008 !caller :0000 child &0003 global\n"
      "488D05........C3 00 0000 0008 !caller :0000 rival &0003 other\n";
  for (bool Reverse : {false, true}) {
    SignatureDB DB;
    const auto Anchor = root() + root("anchor", true);
    const auto Context = Parent + Source + Children;
    add(DB, Reverse ? Context + Anchor : Anchor + Context);
    const auto Entries = Reverse
                             ? std::vector<uint64_t>{0x1100, 0x1080, 0x1000}
                             : std::vector<uint64_t>{0x1000, 0x1080, 0x1100};
    DB.apply(callerImage(), Entries);
    const auto Names = DB.buildCandidateNameMap();
    ASSERT_TRUE(Names.contains(0x1100));
    EXPECT_EQ(Names.at(0x1100), "child");
    EXPECT_EQ(Names.at(0x1080), "parent");
    EXPECT_EQ(Names.at(0x1000), "anchor");
    EXPECT_TRUE(DB.findMatch(0x1100)->CallerConfirmed);
    EXPECT_TRUE(DB.findMatch(0x1100)->Confirmed);
    EXPECT_TRUE(DB.buildNameMap().empty());
    auto Changed = callerImage();
    displacement(Changed, 0x1103, 0x1107, 0x2040);
    DB.apply(Changed, Entries);
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
    DB.apply(callerImage(), Entries);
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "child");
  }
}

TEST(SignatureDataContext, DeferredCallersRetainUnknownAndConflictingEvidence) {
  for (unsigned Case = 0; Case < 11; ++Case) {
    auto Img = callerImage();
    std::string Anchor = root() + root("anchor", true);
    std::string Parent = ParentBody + " 00 0000 001F :0000 parent\n";
    std::string Source = dataParent(true, " ^001A child ^001A rival");
    std::string Children =
        "488D05........C3 00 0000 0008 !caller :0000 child &0003 global\n"
        "488D05........C3 00 0000 0008 !caller :0000 rival &0003 other\n";
    std::vector<uint64_t> Entries{0x1000, 0x1080, 0x1100};
    switch (Case) {
    case 0:
      Anchor = root("anchor", true);
      break;
    case 1:
      Anchor = root();
      break;
    case 2:
      Anchor += root("anchor", true, "");
      break;
    case 3:
      Children += "488D05........C3 00 0000 0008 !caller :0000 rival\n";
      break;
    case 4:
      Children =
          "488D05........C3 00 0000 0008 !caller :0000 child &0003 global\n";
      break;
    case 5:
      Parent += ParentBody + " 00 0000 001F :0000 unknown_parent\n";
      break;
    case 6:
      Source += dataParent(true, "");
      break;
    case 7:
      Entries.pop_back();
      break;
    case 8: {
      const std::string Weak = std::string(32, '.') + ParentBody.substr(32);
      Parent = Weak + " 00 0000 001F :0000 parent\n";
      Source = dataParent(true, " ^001A child ^001A rival", Weak);
      break;
    }
    case 9:
      Img.Segments.back().Flags = SegmentFlags::Writable;
      break;
    case 10:
      Anchor += root("anchor", true, " &0013 other");
      break;
    }
    SignatureDB DB;
    add(DB, Anchor + Parent + Source + Children);
    DB.apply(Img, Entries);
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Case;
    EXPECT_TRUE(DB.buildNameMap().empty());
  }
}

TEST(SignatureDataContext,
     DeferredCallerAlternativesNeedIndependentDataStorage) {
  for (bool Local : {false, true}) {
    const std::string Field = Local ? "%" : "&";
    const std::string First = Local ? "00000013:anchor" : "global";
    const std::string Second = Local ? "00000013:another" : "other";
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " " + Field + "0013 " + Second) +
                ParentBody + " 00 0000 001F :0000 parent\n" +
                dataParent(true, " ^001A child ^001A rival") +
                "488D05........C3 00 0000 0008 !caller :0000 child " + Field +
                "0003 " + First + "\n" +
                "488D05........C3 00 0000 0008 !caller :0000 rival " + Field +
                "0003 " + Second + "\n");
    DB.apply(callerImage(), {0x1000, 0x1080, 0x1100});
    if (Local)
      EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
    else
      EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "rival");
    EXPECT_TRUE(DB.buildNameMap().empty());
  }
}

TEST(SignatureDataContext, LocalDataUsesTheExactIndependentAnchorOperand) {
  const std::string Relation = "00000013:anchor";
  SignatureDB DB;
  add(DB, root() + root("anchor", true, " %0013 " + Relation) +
              getter("getter", "%0003 " + Relation) +
              getter("rival", "%0003 00000013:another_anchor"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
  EXPECT_TRUE(DB.buildNameMap().empty());
  auto Changed = image();
  displacement(Changed, 0x1103, 0x1107, 0x2030);
  DB.apply(Changed, {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, ConfirmedDataParentsCanRequestExactCallerBodies) {
  for (bool Reverse : {false, true}) {
    SignatureDB DB;
    const auto Anchor = root() + root("anchor", true, " %0013 00000013:anchor");
    const auto Context = dataParent() + dataParent(true) + Child;
    add(DB, Reverse ? Context + Anchor : Anchor + Context);
    DB.apply(callerImage(),
             Reverse ? std::vector<uint64_t>{0x1100, 0x1080, 0x1000}
                     : std::vector<uint64_t>{0x1000, 0x1080, 0x1100});
    const auto Names = DB.buildCandidateNameMap();
    ASSERT_TRUE(Names.contains(0x1100));
    EXPECT_EQ(Names.at(0x1100), "child");
    EXPECT_EQ(Names.at(0x1080), "parent");
    EXPECT_EQ(Names.at(0x1000), "anchor");
    EXPECT_TRUE(DB.findMatch(0x1100)->CallerConfirmed);
    EXPECT_TRUE(DB.findMatch(0x1080)->Confirmed);
    EXPECT_FALSE(DB.findMatch(0x1080)->CallerConfirmed);
    EXPECT_FALSE(DB.findMatch(0x1000)->Confirmed);
    EXPECT_TRUE(DB.buildNameMap().empty());
  }
}

TEST(SignatureDataContext, CallerRequestsRetainIndependentRootAndSourceChecks) {
  for (unsigned Case = 0; Case < 8; ++Case) {
    auto Img = callerImage();
    std::string Anchor =
        root() + root("anchor", true, " %0013 00000013:anchor");
    std::string Parent = dataParent();
    std::string Source = dataParent(true);
    std::string Targets = Child;
    std::vector<uint64_t> Entries{0x1000, 0x1080, 0x1100};
    switch (Case) {
    case 0:
      Anchor = root("anchor", true, " %0013 00000013:anchor");
      break;
    case 1: {
      const auto Weak = std::string(32, '.') + ParentBody.substr(32);
      Parent = dataParent(false, "", Weak);
      Source = dataParent(true, " %0013 00000013:anchor ^001A child", Weak);
      break;
    }
    case 2:
      Source.clear();
      break;
    case 3:
      Source += dataParent(true, " %0013 00000013:anchor");
      break;
    case 4:
      Source = dataParent(true, " %0013 00000013:other ^001A child");
      break;
    case 5:
      Targets += "488D05........C3 00 0000 0008 !caller :0000 rival\n";
      Source =
          dataParent(true, " %0013 00000013:anchor ^001A child ^001A rival");
      break;
    case 6:
      displacement(Img, 0x1093, 0x1097, 0x2040);
      break;
    case 7:
      Entries.pop_back();
      break;
    }
    SignatureDB DB;
    add(DB, Anchor + Parent + Source + Targets);
    DB.apply(Img, Entries);
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Case;
    EXPECT_TRUE(DB.buildNameMap().empty());
  }
}

TEST(SignatureDataContext, CallerDescendantsCannotCreateAnotherDataBinding) {
  auto Img = callerImage();
  auto &Code = Img.Segments.front().Data;
  std::fill_n(Code.begin() + 0x180, 16, 0x92);
  const uint8_t Load[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0, 0xc3};
  std::copy(std::begin(Load), std::end(Load), Code.begin() + 0x190);
  displacement(Img, 0x1193, 0x1197, 0x2040);
  displacement(Img, 0x109a, 0x109e, 0x1180);
  displacement(Img, 0x1103, 0x1107, 0x2040);
  SignatureDB DB;
  add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
              dataParent() + dataParent(true) +
              "92929292929292929292929292929292488B05........C3 00 0000 0018 "
              "!caller :0000 child &0013 second\n" +
              getter("getter", "&0003 second"));
  DB.apply(Img, {0x1000, 0x1080, 0x1100, 0x1180});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1180), "child");
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureDataContext, CallerQueriesSettleAllFrozenClaimsAtTheTarget) {
  for (bool Ambiguous : {false, true}) {
    auto Img = callerImage();
    auto &Code = Img.Segments.front().Data;
    std::fill_n(Code.begin() + 0x100, 16, 0x92);
    Code[0x110] = 0xc3;
    const std::string Body = "92929292929292929292929292929292C3 00 0000 0011";
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
                dataParent() + dataParent(true) + Body +
                " !caller :0000 child\n" + Body + " :0000 other_a\n" +
                (Ambiguous ? Body + " :0000 other_b\n" : ""));
    DB.apply(Img, {0x1100, 0x1080, 0x1000});
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100),
              Ambiguous ? "child" : "other_a");
    EXPECT_TRUE(DB.buildNameMap().empty());
  }
}

TEST(SignatureDataContext, DataRootedChainsDoNotRevisitTheirParents) {
  auto Img = callerImage();
  auto &Code = Img.Segments.front().Data;
  std::fill_n(Code.begin() + 0x180, 16, 0x92);
  const uint8_t Address[] = {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3};
  std::copy(std::begin(Address), std::end(Address), Code.begin() + 0x190);
  displacement(Img, 0x1193, 0x1197, 0x1100);
  displacement(Img, 0x109a, 0x109e, 0x1180);
  displacement(Img, 0x1103, 0x1107, 0x1080);
  SignatureDB DB;
  add(DB,
      root() + root("anchor", true, " %0013 00000013:anchor") + dataParent() +
          dataParent(true) +
          "92929292929292929292929292929292488D05........C3 00 0000 0018 "
          "!caller :0000 child ^0013 leaf\n"
          "488D05........C3 00 0000 0008 !caller :0000 leaf ^0003 parent\n");
  DB.apply(Img, {0x1180, 0x1100, 0x1080, 0x1000});
  const auto Names = DB.buildCandidateNameMap();
  EXPECT_EQ(Names.size(), 4u);
  EXPECT_EQ(Names.at(0x1180), "child");
  EXPECT_EQ(Names.at(0x1100), "leaf");
  EXPECT_FALSE(DB.findMatch(0x1080)->CallerConfirmed);
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureDataContext, LocalScopeCannotBeForgedByAnotherNameSiteOrKind) {
  for (const auto &Ref : {" %0013 00000013:another_anchor",
                          " %0013 00000003:anchor", " &0013 00000013:anchor"}) {
    SignatureDB DB;
    add(DB, root() + root("anchor", true, Ref) +
                getter("getter", "%0003 00000013:anchor"));
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Ref;
  }
  SignatureDB DB;
  add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
              getter("getter", "&0003 00000013:anchor"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, LocalRelationsRetainAllVariantAndParentConstraints) {
  for (const auto &Other : {"", " %0013 00000013:other",
                            " %0013 00000013:anchor %0013 00000014:anchor"}) {
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
                root("anchor", true, Other) +
                getter("getter", "%0003 00000013:anchor"));
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
  SignatureDB DB;
  add(DB, root("anchor", true, " %0013 00000013:anchor") +
              getter("getter", "%0003 00000013:anchor"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_TRUE(DB.buildCandidateNameMap().empty());
}

TEST(SignatureDataContext, LocalConsumerAlternativesNeedAnExactBoundOperand) {
  for (bool Reverse : {false, true}) {
    const std::string Exact = "%0003 00000013:anchor";
    const std::string Other = "%0003 00000014:anchor";
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
                getter("getter",
                       Reverse ? Other + " " + Exact : Exact + " " + Other));
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
    EXPECT_TRUE(DB.buildNameMap().empty());
    auto Changed = image();
    displacement(Changed, 0x1103, 0x1107, 0x2030);
    DB.apply(Changed, {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
}

TEST(SignatureDataContext, TwoLocationsForOneLocalAnchorRemainAmbiguous) {
  SignatureDB DB;
  add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
              getter("getter", "%0003 00000013:anchor"));
  auto Img = image();
  std::copy_n(Img.Segments.front().Data.begin(), 24,
              Img.Segments.front().Data.begin() + 0x200);
  displacement(Img, 0x1213, 0x1217, 0x2030);
  DB.apply(Img, {0x1000, 0x1100, 0x1200});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, LayoutQueriesFrozenOperandsInEitherDirection) {
  for (bool Reverse : {false, true}) {
    for (bool Negative : {false, true}) {
      auto Img = image();
      displacement(Img, 0x1103, 0x1107, Negative ? 0x2010 : 0x2030);
      const std::string Exact =
          Negative ? "00000013-00000010:anchor" : "00000013+00000010:anchor";
      const std::string Other = "00000013+00000020:anchor";
      SignatureDB DB;
      add(DB,
          root() + root("anchor", true, " %0013 00000013:anchor") +
              getter("getter", Reverse ? "%0003 " + Other + " %0003 " + Exact
                                       : "%0003 " + Exact + " %0003 " + Other));
      DB.apply(Img, Reverse ? std::vector<uint64_t>{0x1100, 0x1000}
                            : std::vector<uint64_t>{0x1000, 0x1100});
      EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
      EXPECT_TRUE(DB.buildNameMap().empty());
      displacement(Img, 0x1103, 0x1107, 0x2050);
      DB.apply(Img, {0x1000, 0x1100});
      EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
    }
  }
}

TEST(SignatureDataContext, LayoutCannotBindFromRelativeOrConflictingRoots) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    auto Img = image();
    displacement(Img, 0x1103, 0x1107, 0x2030);
    std::string Refinement = root("anchor", true, " %0013 00000013:anchor");
    if (Case == 0)
      Refinement = root("anchor", true, " %0013 00000013+00000010:anchor");
    if (Case == 1)
      Refinement += root("anchor", true, "");
    if (Case == 2)
      Refinement += root("anchor", true, " %0013 00000013:another");
    if (Case == 3) {
      std::copy_n(Img.Segments.front().Data.begin(), 24,
                  Img.Segments.front().Data.begin() + 0x200);
      displacement(Img, 0x1213, 0x1217, 0x2040);
    }
    SignatureDB DB;
    add(DB, root() + Refinement +
                getter("getter", "%0003 00000013+00000010:anchor"));
    DB.apply(Img, {0x1000, 0x1100, 0x1200});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Case;
  }
}

TEST(SignatureDataContext, LayoutCannotCrossMappedRegionsOrBorrowExtent) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    auto Img = image();
    displacement(Img, 0x1103, 0x1107, 0x2050);
    if (Case == 0) {
      Img.Segments.back().Size = 0x40;
      auto Other = Img.Segments.back();
      Other.VA += Other.Size;
      Img.Segments.push_back(Other);
    } else if (Case == 1) {
      Section First;
      First.VA = 0x2000;
      First.Size = 0x40;
      First.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      auto Second = First;
      Second.VA += First.Size;
      Img.Sections = {First, Second};
    } else if (Case == 2) {
      Img.Segments.back().Size = 0x50;
    } else {
      auto Other = Img.Segments.back();
      Other.VA = 0x2050;
      Other.Size = 1;
      Img.Segments.push_back(Other);
    }
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " %0013 00000013:anchor") +
                getter("getter", "%0003 00000013+00000030:anchor"));
    DB.apply(Img, {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Case;
  }
}

TEST(SignatureDataContext, LayoutArithmeticDoesNotWrapOrCreateAnAnchor) {
  auto Img = image();
  const auto Positive = localDataAnchor("00000013+FFFFFFFF:anchor");
  const auto Negative = localDataAnchor("00000013-FFFFFFFF:anchor");
  ASSERT_TRUE(Positive);
  ASSERT_TRUE(Negative);
  EXPECT_FALSE(localDataReferenceBase(Img, 0x2020, *Positive));
  EXPECT_FALSE(localDataReferenceBase(Img, InvalidVA - 0x10, *Negative));
  // A relative reference to a different function's operand cannot turn
  // a newly proposed body into an independent root for a second relation.
  auto &Code = Img.Segments.front().Data;
  std::copy_n(Code.begin(), 24, Code.begin() + 0x80);
  Code[0x80] = 0x91;
  displacement(Img, 0x1093, 0x1097, 0x2030);
  displacement(Img, 0x1103, 0x1107, 0x2040);
  const auto Derived = "91" + Body.substr(2);
  SignatureDB DB;
  auto Consumer =
      root("derived", true, " %0013 00000013+00000010:anchor", Derived);
  Consumer.replace(Consumer.find("!caller"), 7, "!refs");
  add(DB, root() + root("anchor", true, " %0013 00000013:anchor") + Consumer +
              root("derived", true, " %0013 00000013:derived", Derived) +
              getter("getter", "%0003 00000013+00000010:derived"));
  DB.apply(Img, {0x1000, 0x1080, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1080), "derived");
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureDataContext, WeakPartialAmbiguousAndRequiredRootsDoNotBind) {
  for (const auto &Anchor :
       {"90 00 0000 0018 :0000 anchor\n",
        "................................488B05........C3 00 0000 0018 :0000 "
        "anchor\n",
        "90909090909090909090909090909090488B05........C3 00 0000 0018 !refs "
        ":0000 anchor &0013 global\n"}) {
    SignatureDB DB;
    add(DB, std::string(Anchor) + root("anchor", true) + getter());
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
  SignatureDB DB;
  add(DB, root() + root("another") + root("anchor", true) + getter());
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext,
     EveryMatchingSourceMustAgreeIncludingAbsenceAndAlternatives) {
  for (const auto &Other : {"", " &0013 other", " &0013 global &0013 other"}) {
    for (bool Reverse : {false, true}) {
      SignatureDB DB;
      const auto Good = root("anchor", true);
      const auto Bad = root("anchor", true, Other);
      add(DB, root() + (Reverse ? Bad + Good : Good + Bad) + getter());
      DB.apply(image(), {0x1000, 0x1100});
      EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
    }
  }
}

TEST(SignatureDataContext, WeakSourceRefinementsCannotAddDataToAStrongRoot) {
  const std::string Weak = "................................488B05........C3";
  for (bool AddStrongRefinement : {false, true}) {
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " &0013 global", Weak) +
                (AddStrongRefinement ? root("anchor", true) : "") + getter());
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
}

TEST(SignatureDataContext, ConflictingNamesOrLocationsCannotBind) {
  for (bool SameAddress : {false, true}) {
    auto Img = image();
    std::copy_n(Img.Segments.front().Data.begin(), 24,
                Img.Segments.front().Data.begin() + 0x40);
    Img.Segments.front().Data[0x40] = 0x91;
    displacement(Img, 0x1053, 0x1057, SameAddress ? 0x2020 : 0x2040);
    const auto OtherBody = "91" + Body.substr(2);
    SignatureDB DB;
    add(DB,
        root() + root("anchor", true) + root("second", false, "", OtherBody) +
            root("second", true, SameAddress ? " &0013 other" : " &0013 global",
                 OtherBody) +
            getter());
    DB.apply(Img, {0x1000, 0x1040, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
}

TEST(SignatureDataContext, FunctionReferencesNeverAcquireDataSemantics) {
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + getter("getter", "^0003 global"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext,
     DerivedCandidatesCannotEstablishFurtherDataBindings) {
  auto Img = image();
  // A second strong body uses the root's global and takes another address.
  auto &Code = Img.Segments.front().Data;
  std::fill_n(Code.begin() + 0x80, 16, 0x91);
  const uint8_t Operands[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0,   0x48,
                              0x8d, 0x0d, 0,    0, 0, 0, 0xc3};
  std::copy(std::begin(Operands), std::end(Operands), Code.begin() + 0x90);
  displacement(Img, 0x1093, 0x1097, 0x2020);
  displacement(Img, 0x109a, 0x109e, 0x2040);
  displacement(Img, 0x1103, 0x1107, 0x2040);
  const std::string Derived =
      "91919191919191919191919191919191488B05........488D0D........C3 "
      "00 0000 001F ";
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + Derived +
              "!refs :0000 derived &0013 global\n" + Derived +
              "!caller :0000 derived &0013 global &001A second\n" +
              getter("getter", "&0003 second"));
  DB.apply(Img, {0x1000, 0x1080, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1080), "derived");
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureDataContext,
     OneContradictedSourceRefinementInvalidatesTheAnchor) {
  auto Img = image();
  auto &Code = Img.Segments.front().Data;
  Code[0x17] = 0xe8;
  Code[0x1c] = 0xc3;
  displacement(Img, 0x1018, 0x101c, 0x1200);
  Code[0x200] = 0xaa;
  const std::string Called =
      Body.substr(0, Body.size() - 2) + "E8........C3 00 0000 001D ";
  SignatureDB DB;
  add(DB, Called + ":0000 anchor\n" + Called +
              "!caller :0000 anchor &0013 global ^0018 actual\n" + Called +
              "!caller :0000 anchor &0013 global ^0018 contradicted\n" +
              "AA 00 0000 0001 :0000 actual\n" + getter());
  DB.apply(Img, {0x1000, 0x1100, 0x1200});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext,
     RequiresReadableNonexecutingUnambiguousMappedExtent) {
  for (unsigned Case = 0; Case < 11; ++Case) {
    auto Img = image();
    switch (Case) {
    case 0:
      Img.Segments.back().Flags = SegmentFlags::None;
      break;
    case 1:
      Img.Segments.back().Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 2:
      Img.Segments.back().Size = 0x27;
      break; // 8-byte root crosses end.
    case 3:
      Img.Segments.push_back(Img.Segments.back());
      break;
    case 4:
      Img.IsRelocatable = true;
      break;
    case 5:
      Img.Arch = Arch::X86;
      break;
    case 6:
      Img.Format = BinaryFormat::ELF;
      break;
    case 7:
      displacement(Img, 0x1103, 0x1107, 0x2040);
      break;
    case 8: {
      Section Sec;
      Sec.VA = 0x2000;
      Sec.Size = 0x100;
      Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Img.Sections.push_back(Sec);
      break;
    }
    case 9:
      Img.Segments.back().VA = InvalidVA - 0x10;
      break;
    case 10:
      displacement(Img, 0x1013, 0x1017, 0x1000);
      displacement(Img, 0x1103, 0x1107, 0x1000);
      break;
    }
    SignatureDB DB;
    add(DB, root() + root("anchor", true) + getter());
    DB.apply(Img, {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Case;
  }
}

TEST(SignatureDataContext, AnImmediateCannotBorrowBytesPastTheSourceBody) {
  auto Img = image();
  Img.Segments.front().Data[0x100] = 0xc7;
  Img.Segments.front().Data[0x101] = 0x05;
  std::fill_n(Img.Segments.front().Data.begin() + 0x106, 4, 0);
  displacement(Img, 0x1102, 0x110a, 0x2020);
  SignatureDB DB;
  add(DB, root() + root("anchor", true) +
              "C705........ 00 0000 0006 !refs :0000 truncated &0002 global\n");
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, NegativeDisplacementsAndFinalMappedBytesAreValid) {
  auto Img = image();
  Img.Segments.back().VA = 0x800;
  Img.Segments.back().Size = 8;
  displacement(Img, 0x1013, 0x1017, 0x800);
  displacement(Img, 0x1103, 0x1107, 0x800);
  Img.Segments.front().Data.resize(0x108);
  Img.Segments.front().Size = Img.Segments.front().FileSz = 0x108;
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + getter());
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
}

TEST(SignatureDataOperand, ExactImmediateBiasWidthsAndPrefixRefusals) {
  struct Form {
    std::vector<uint8_t> Bytes;
    size_t Field;
    unsigned Width;
    unsigned Bias;
  };
  const Form Forms[] = {{{0xff, 0x15, 0, 0, 0, 0}, 2, 8, 0},
                        {{0xff, 0x25, 0, 0, 0, 0}, 2, 8, 0},
                        {{0x48, 0xff, 0x25, 0, 0, 0, 0}, 3, 8, 0},
                        {{0x48, 0x8b, 0x05, 0, 0, 0, 0}, 3, 8, 0},
                        {{0x4c, 0x89, 0x3d, 0, 0, 0, 0}, 3, 8, 0},
                        {{0x48, 0x8d, 0x0d, 0, 0, 0, 0}, 3, 1, 0},
                        {{0x8b, 0x05, 0, 0, 0, 0}, 2, 4, 0},
                        {{0x83, 0x3d, 0, 0, 0, 0, 0}, 2, 4, 1},
                        {{0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0}, 2, 4, 4},
                        {{0x48, 0x81, 0x3d, 0, 0, 0, 0, 0, 0, 0, 0}, 3, 8, 4}};
  for (const auto &Form : Forms) {
    auto Field = x64DataReferenceField(Form.Bytes, Form.Field);
    ASSERT_TRUE(Field);
    EXPECT_EQ(Field->Width, Form.Width);
    EXPECT_EQ(Field->Bias, Form.Bias);
    EXPECT_EQ(Field->End, Form.Bytes.size());
    auto Truncated = Form.Bytes;
    Truncated.pop_back();
    EXPECT_FALSE(x64DataReferenceField(Truncated, Form.Field));
    for (uint8_t Prefix : {0x66, 0x67, 0x64, 0xf0, 0xf2, 0xf3, 0x41, 0x48}) {
      auto Prefixed = Form.Bytes;
      Prefixed.insert(Prefixed.begin(), Prefix);
      // A single REX.W is supported on a previously unprefixed form.
      if (Prefix == 0x48 && Form.Field == 2)
        continue;
      EXPECT_FALSE(x64DataReferenceField(Prefixed, Form.Field + 1));
    }
  }
  EXPECT_FALSE(x64DataReferenceField({0x4c, 0xff, 0x15, 0, 0, 0, 0}, 3));
  EXPECT_FALSE(x64DataReferenceField({0x8d, 0x05, 0, 0, 0, 0}, 2));
  EXPECT_FALSE(x64DataReferenceField({0x48, 0x8b, 0x45, 0, 0, 0, 0}, 3));
}
