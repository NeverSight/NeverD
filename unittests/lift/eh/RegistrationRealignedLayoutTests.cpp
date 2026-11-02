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

bool proveRealignedCxxRegistrationLayout(const BinaryImage &Image,
                                         const InstallSite &Site,
                                         RegistrationChainInfo &Chain) {
  const auto Layout = decodeRealignedCxxRegistrationLayout(Image, Site);
  if (Layout) {
    EXPECT_EQ(Layout->HandlerPointerVA, Layout->ChainReadVA - 4);
    EXPECT_EQ(readLE<uint32_t>(Image.readVA(Layout->HandlerPointerVA, 4)),
              Site.HandlerVA);
  }
  return Layout && Layout->applyTo(Site.InstallVA, Chain);
}

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
        // A PE section and the segment that maps it share one set of
        // characteristics.
        Text.Flags = SegmentFlags::Readable;
        F.Image.Sections[0].Flags = SegmentFlags::Readable;
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

TEST(RegistrationRealignedLayout, RequiresTheWholeESIPrefixStorage) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    RealignedLayoutImage F({}, 3, 7, true);
    auto &Text = F.Image.Segments[0];
    if (Mutation == 0) {
      Segment Alias = Text;
      Alias.Size = Alias.FileSz = 6;
      Alias.Data.resize(6);
      F.Image.Segments.push_back(std::move(Alias));
    } else if (Mutation == 1)
      F.Image.BaseRelocations.push_back({Text.VA + 8, 3});
    else {
      Section Alias = F.Image.Sections[0];
      Alias.Size = Alias.FileSz = 6;
      Alias.Flags = Alias.Flags | SegmentFlags::Writable;
      F.Image.Sections.push_back(std::move(Alias));
    }
    EXPECT_FALSE(F.prove());
  }
  RealignedLayoutImage Handler({}, 3, 7, true);
  Handler.Image.BaseRelocations.push_back({Handler.Site.Range.Begin + 36, 3});
  EXPECT_TRUE(Handler.prove());
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
      EXPECT_FALSE(disprovesCallbackBoundary(F.Image, Text.VA, Boundary, Work));
      EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

struct StackRealignedLayoutImage {
  BinaryImage Image;
  InstallSite Site;
  va_t Publication = 0;
  uint32_t Depth = 0;

  StackRealignedLayoutImage(unsigned Alignment, unsigned Allocation,
                            llvm::ArrayRef<uint8_t> Saved, bool Cookie,
                            bool Wide, bool ModRM) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.VA = 0x401000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    auto Add = [&](llvm::ArrayRef<uint8_t> Bytes) {
      Text.Data.insert(Text.Data.end(), Bytes.begin(), Bytes.end());
    };
    auto Word = [&](uint32_t Value) {
      for (unsigned I = 0; I != 4; ++I)
        Text.Data.push_back(uint8_t(Value >> (8 * I)));
    };
    Add({0x55, 0x8b, 0xec, 0x83, 0xe4, uint8_t(-Alignment), 0x6a, 0xff, 0x68});
    Word(0x402000);
    Site.InstallVA = Text.VA + Text.Data.size();
    Add({0x64, 0xa1, 0, 0, 0, 0, 0x50});
    if (Wide) {
      Add({0x81, 0xec});
      Word(Allocation);
    } else
      Add({0x83, 0xec, uint8_t(Allocation)});
    Add(Saved);
    if (Cookie)
      Add({0xa1, 0, 0x30, 0x40, 0, 0x33, 0xc4, 0x50});
    Depth = 12 + Allocation + 4 * Saved.size() + (Cookie ? 4 : 0);
    if (Wide) {
      Add({0x8d, 0x84, 0x24});
      Word(Depth - 12);
    } else
      Add({0x8d, 0x44, 0x24, uint8_t(Depth - 12)});
    Publication = Text.VA + Text.Data.size();
    if (ModRM)
      Add({0x64, 0x89, 0x05, 0, 0, 0, 0});
    else
      Add({0x64, 0xa3, 0, 0, 0, 0});
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
};

TEST(RegistrationRealignedLayout, ProvesStackBasedMSVCNodeIndependentlyOfEBP) {
  const std::vector<std::vector<uint8_t>> Saved = {
      {}, {0x53}, {0x57, 0x53, 0x56}};
  for (unsigned Alignment : {4u, 8u, 16u, 64u, 128u})
    for (const auto &Registers : Saved)
      for (bool Cookie : {false, true})
        for (bool Wide : {false, true})
          for (bool ModRM : {false, true}) {
            StackRealignedLayoutImage F(Alignment, Wide ? 512 : 48, Registers,
                                        Cookie, Wide, ModRM);
            RegistrationChainInfo Chain;
            ASSERT_TRUE(
                proveRealignedCxxRegistrationLayout(F.Image, F.Site, Chain));
            ASSERT_TRUE(Chain.RealignedFrame);
            const auto &Frame = *Chain.RealignedFrame;
            EXPECT_EQ(Frame.BaseRegister, 4u);
            EXPECT_EQ(Frame.DefinitionVA, F.Site.Range.Begin + 3);
            EXPECT_EQ(Frame.Alignment, Alignment);
            EXPECT_EQ(Frame.AllocationBytes, F.Depth);
            EXPECT_EQ(Frame.BaseOffset, -int32_t(F.Depth));
            EXPECT_EQ(Frame.SavedParentFrameOffset, 0);
            EXPECT_EQ(Chain.RegistrationOffset, -12);
            EXPECT_EQ(Chain.TryLevelOffset, -4);
            EXPECT_EQ(Chain.ChainInstallVA, F.Publication);
          }
}

TEST(RegistrationRealignedLayout, RejectsChangedStackPublicationAndOwnership) {
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StackRealignedLayoutImage F(16, 48, {0x53, 0x56, 0x57}, true, false, false);
    auto &Text = F.Image.Segments[0];
    const size_t Publication = F.Publication - Text.VA;
    switch (Mutation) {
    case 0:
      Text.Data[5] = 0xe8; // Non-power-of-two alignment.
      break;
    case 1:
      Text.Data[5] = 0xfe; // Unsupported two-byte alignment.
      break;
    case 2:
      Text.Data[7] = 0;
      break;
    case 3:
      ++Text.Data[9]; // A different handler.
      break;
    case 4:
      Text.Data[13] = 0x65;
      break;
    case 5:
      Text.Data[15] = 1;
      break;
    case 6:
      Text.Data[19] = 0x51; // Push the wrong chain link.
      break;
    case 7:
      Text.Data[22] = 0xff; // SUB would move ESP upward.
      break;
    case 8:
      Text.Data[24] = 0x53; // Repeated preserved-register push.
      break;
    case 9:
      Text.Data[32] = 0xc5; // Cookie depends on entry EBP.
      break;
    case 10:
      Text.Data[33] = 0x51;
      break;
    case 11:
      Text.Data[Publication - 3] = 0x45; // EBP-based LEA.
      break;
    case 12:
      Text.Data[Publication - 2] = 0x04; // Indexed SIB.
      break;
    case 13:
      ++Text.Data[Publication - 1];
      break;
    case 14:
      ++F.Site.InstallVA;
      break;
    case 15:
      Text.Data.back() = 1;
      break;
    case 16:
      --F.Image.KnownCodeRanges[0].second;
      --F.Image.Sections[0].Size;
      --F.Image.Sections[0].FileSz;
      break;
    case 17:
      Text.Data.pop_back();
      break;
    case 18:
      Text.Flags = Text.Flags | SegmentFlags::Writable;
      break;
    case 19: {
      Segment Alias = Text;
      Alias.Size = Alias.FileSz = 6;
      Alias.Data.resize(6);
      F.Image.Segments.push_back(std::move(Alias));
      break;
    }
    case 20:
      F.Image.Arch = Arch::X64;
      break;
    case 21:
      F.Image.Format = BinaryFormat::ELF;
      break;
    }
    RegistrationChainInfo Chain;
    EXPECT_FALSE(proveRealignedCxxRegistrationLayout(F.Image, F.Site, Chain));
    EXPECT_FALSE(Chain.RealignedFrame);
    EXPECT_FALSE(Chain.RegistrationOffset);
    EXPECT_FALSE(Chain.TryLevelOffset);
    EXPECT_EQ(Chain.ChainInstallVA, 0u);
  }
}
} // namespace
