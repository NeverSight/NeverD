//===- RegistrationRealignedLayoutTests.cpp - PE32 publication ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove realigned frame coordinates independently of register allocation.
//===----------------------------------------------------------------------===//

#include "../../../lib/loader/COFF/eh/COFFRegistrationEHDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace {
using namespace neverd;
using namespace neverd::coff_loader;
using namespace neverd::coff_loader::registration_detail;

struct RealignedLayoutImage {
  BinaryImage Image;
  InstallSite Site;
  va_t Publication = 0;
  uint8_t PublicationSize = 0;

  RealignedLayoutImage(llvm::ArrayRef<uint8_t> Arguments, unsigned NodeRegister,
                       unsigned LinkRegister, bool ModRM,
                       bool MoffsRead = false) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.VA = 0x401000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {0x55, 0x89, 0xe5, 0x53, 0x57, 0x56, 0x83, 0xe4, 0xf0,
                 0x81, 0xec, 0x40, 0,    0,    0,    0x89, 0xe6};
    auto Add = [&](llvm::ArrayRef<uint8_t> Bytes) {
      Text.Data.insert(Text.Data.end(), Bytes.begin(), Bytes.end());
    };
    Add({0x89, 0x6e, 0x28}); // Saved entry EBP.
    Add(Arguments);
    Add({0x89, 0x66, 0x2c}); // Saved ESP.
    Add({0xc7, 0x46, 0x38, 0xff, 0xff, 0xff, 0xff});
    Add({0x8d, uint8_t(0x46 | NodeRegister << 3), 0x30});
    Add({0xc7, 0x46, 0x34, 0, 0x20, 0x40, 0});
    Site.InstallVA = Text.VA + Text.Data.size();
    if (MoffsRead)
      Add({0x64, 0xa1, 0, 0, 0, 0});
    else
      Add({0x64, 0x8b, uint8_t(5 | LinkRegister << 3), 0, 0, 0, 0});
    Add({0x89, uint8_t(0x46 | LinkRegister << 3), 0x30});
    Publication = Text.VA + Text.Data.size();
    if (ModRM)
      Add({0x64, 0x89, uint8_t(5 | NodeRegister << 3), 0, 0, 0, 0});
    else
      Add({0x64, 0xa3, 0, 0, 0, 0});
    PublicationSize = Text.VA + Text.Data.size() - Publication;
    Text.Size = Text.FileSz = Text.Data.size();
    Site.Range = {Text.VA, Text.VA + Text.Size};
    Site.HandlerVA = 0x402000;
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections.push_back(Code);
    Image.KnownCodeRanges.emplace_back(Site.Range.Begin, Site.Range.End);
    Image.Segments.push_back(std::move(Text));
  }

  bool prove() {
    RegistrationChainInfo Chain;
    const bool Result = proveRealignedCxxRegistrationLayout(Image, Site, Chain);
    if (Result) {
      EXPECT_EQ(Chain.RegistrationOffset, -12);
      EXPECT_EQ(Chain.TryLevelOffset, -4);
      EXPECT_EQ(Chain.ChainInstallVA, Publication);
      EXPECT_EQ(getX86RegistrationChainStoreSize(Image, Publication),
                PublicationSize);
      EXPECT_TRUE(Chain.RealignedFrame);
      if (Chain.RealignedFrame) {
        EXPECT_EQ(Chain.RealignedFrame->BaseRegister, 6u);
        EXPECT_EQ(Chain.RealignedFrame->AllocationBytes, 0x40u);
        EXPECT_EQ(Chain.RealignedFrame->SavedParentFrameOffset, -20);
      }
    } else {
      EXPECT_FALSE(Chain.RegistrationOffset);
      EXPECT_FALSE(Chain.TryLevelOffset);
      EXPECT_FALSE(Chain.ChainInstallVA);
      EXPECT_FALSE(Chain.RealignedFrame);
    }
    return Result;
  }
};

TEST(RegistrationRealignedLayout, AcceptsIndependentNodeAndLinkRegisters) {
  const std::vector<std::vector<uint8_t>> Arguments = {
      {},
      {0x8b, 0x45, 12},
      {0x8b, 0x85, 0x80, 0, 0, 0},
      {0x8b, 0x4d, 8, 0x8b, 0x7d, 12},
      {0x8b, 0x45, 12, 0x89, 0x46, 0x20},
      {0x8b, 0x45, 12, 0x89, 0x86, 0x20, 0, 0, 0, 0x8b, 0x55, 8}};
  for (const auto &Prefix : Arguments)
    for (unsigned Node : {0u, 1u, 2u, 3u, 7u})
      for (unsigned Link : {0u, 1u, 2u, 3u, 7u})
        for (bool ModRM : {false, true}) {
          if (Node == Link || (!ModRM && Node != 0))
            continue;
          RealignedLayoutImage F(Prefix, Node, Link, ModRM);
          EXPECT_TRUE(F.prove());
          if (Link == 0) {
            RealignedLayoutImage Moffs(Prefix, Node, Link, ModRM, true);
            EXPECT_TRUE(Moffs.prove());
          }
        }
}

TEST(RegistrationRealignedLayout, RejectsClobberedCoordinatesAndNodeAliases) {
  for (unsigned Node = 0; Node != 8; ++Node)
    for (unsigned Link = 0; Link != 8; ++Link) {
      if (Node != Link && Node != 4 && Node != 5 && Node != 6 && Link != 4 &&
          Link != 5 && Link != 6)
        continue;
      RealignedLayoutImage F({}, Node, Link, true);
      EXPECT_FALSE(F.prove());
    }
  const std::vector<std::vector<uint8_t>> Arguments = {
      {0x8b, 0x65, 8},    {0x8b, 0x6d, 8},    {0x8b, 0x75, 8}, {0x8b, 0x45, 4},
      {0x8b, 0x45, 0xfc}, {0x89, 0x4e, 0x38}, {0x90},          {0x8b, 0x45}};
  for (const auto &Prefix : Arguments) {
    RealignedLayoutImage F(Prefix, 3, 7, true);
    EXPECT_FALSE(F.prove());
  }
  for (unsigned Byte : {19u, 22u, 25u, 26u, 32u, 35u, 36u}) {
    RealignedLayoutImage F({}, 3, 7, true);
    ++F.Image.Segments[0].Data[Byte];
    EXPECT_FALSE(F.prove());
  }
  RealignedLayoutImage WrongSite({}, 3, 7, true);
  ++WrongSite.Site.InstallVA;
  EXPECT_FALSE(WrongSite.prove());
  RealignedLayoutImage WrongStore({}, 3, 7, false);
  EXPECT_FALSE(WrongStore.prove());
}

TEST(RegistrationRealignedLayout, RequiresOwnedCompletePublicationBytes) {
  for (bool ModRM : {false, true})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SCOPED_TRACE(ModRM);
      RealignedLayoutImage F({}, 0, 7, ModRM);
      auto &Text = F.Image.Segments[0];
      const size_t Offset = F.Publication - Text.VA;
      switch (Mutation) {
      case 0:
        Text.Data[Offset] = 0x65;
        break;
      case 1:
        Text.Data.back() = 1;
        break;
      case 2:
        Text.Data.pop_back();
        --Text.Size;
        --Text.FileSz;
        break;
      case 3:
        --F.Image.KnownCodeRanges[0].second;
        --F.Image.Sections[0].Size;
        --F.Image.Sections[0].FileSz;
        break;
      case 4:
        Text.Flags = SegmentFlags::Readable;
        break;
      case 5:
        F.Image.Bits = Bitness::Bits64;
        break;
      case 6:
        F.Image.Arch = Arch::ARM;
        break;
      case 7:
        F.Image.Format = BinaryFormat::ELF;
        break;
      }
      EXPECT_FALSE(getX86RegistrationChainStoreSize(F.Image, F.Publication));
      EXPECT_FALSE(F.prove());
    }
}
TEST(RegistrationRealignedLayout, ParentOwnsStraightLinePaddingLookalikes) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RealignedLayoutImage F({}, 3, 7, true);
    auto &Text = F.Image.Segments[0];
    if (Mutation >= 5)
      Text.Data.insert(Text.Data.end(), 8192, 0x90);
    const unsigned Shape = Mutation % 5;
    const size_t Body = Text.Data.size();
    const uint8_t Store[] = {0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0};
    Text.Data.insert(Text.Data.end(), std::begin(Store), std::end(Store));
    Text.Data.push_back(0xc3);
    Text.Size = Text.FileSz = Text.Data.size();
    F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
    F.Image.KnownCodeRanges[0].second = Text.VA + Text.Size;
    F.Image.Entry = Text.VA;
    const va_t Boundary = Text.VA + Body + 3;
    auto Guess = Symbol::makeFunc(Boundary);
    Guess.IsBoundaryGuess = true;
    if (Shape == 1)
      Guess.Name = "stated_function";
    if (Shape == 2)
      Text.Data[Body] = 0xc3;
    F.Image.Symbols.push_back(Guess);
    if (Shape == 4)
      F.Image.Symbols.push_back(Symbol::makeFunc(Boundary));
    RegistrationChainInfo Chain;
    ASSERT_TRUE(proveRealignedCxxRegistrationLayout(F.Image, F.Site, Chain));
    if (Shape == 3)
      Chain.RegistrationOffset.reset();
    ExceptionFunction EH;
    EH.CodeRange = {Text.VA, Boundary};
    recoverRegistrationCallbackRanges(EH, F.Image, FunctionRangeMap(F.Image),
                                      Chain);
    EXPECT_EQ(EH.CodeRange.End, Shape == 0 ? Text.VA + Text.Size : Boundary);
    for (size_t Remaining : {0, 2}) {
      size_t Work = limits::kMaxRegistrationEHStateWork - Remaining;
      EXPECT_FALSE(
          callbackFallsThroughBoundary(F.Image, Text.VA, Boundary, Work));
      EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}
} // namespace
