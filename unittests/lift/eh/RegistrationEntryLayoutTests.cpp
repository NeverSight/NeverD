//===- RegistrationEntryLayoutTests.cpp - PE32 parameter prologues -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check that argument setup cannot impersonate or overwrite runtime cells.
//===----------------------------------------------------------------------===//
#include "../../../lib/loader/COFF/eh/COFFRegistrationEHDetail.h"
#include "gtest/gtest.h"

namespace {
using namespace neverd;
using namespace neverd::coff_loader::registration_detail;

struct EntryLayoutImage {
  BinaryImage Image;
  InstallSite Site;
  size_t SavedSPStore = 0;
  size_t NodeAddress = 0;
  size_t LinkStore = 0;
  size_t Publication = 0;

  EntryLayoutImage(llvm::ArrayRef<uint8_t> Arguments, bool EarlyNode,
                   unsigned LinkRegister = 1, unsigned NodeRegister = 0,
                   unsigned SavedSPRegister = 4) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.VA = 0x401000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {0x55, 0x89, 0xe5, 0x53, 0x57, 0x56, 0x83, 0xec, 0x30};
    auto Add = [&](llvm::ArrayRef<uint8_t> Bytes) {
      Text.Data.insert(Text.Data.end(), Bytes.begin(), Bytes.end());
    };
    Add(Arguments);
    if (SavedSPRegister != 4)
      Add({0x89, uint8_t(0xe0 | SavedSPRegister)});
    SavedSPStore = Text.Data.size();
    Add({0x89, uint8_t(0x45 | SavedSPRegister << 3), 0xe4, 0xc7, 0x45, 0xf0,
         0xff, 0xff, 0xff, 0xff});
    auto AddNodeAddress = [&] {
      NodeAddress = Text.Data.size();
      Add({0x8d, uint8_t(0x45 | NodeRegister << 3), 0xe8});
    };
    if (EarlyNode)
      AddNodeAddress();
    Add({0xc7, 0x45, 0xec, 0x00, 0x20, 0x40, 0x00});
    Site.InstallVA = Text.VA + Text.Data.size();
    if (LinkRegister != 0)
      Add({0x64, 0x8b, uint8_t(5 | LinkRegister << 3), 0, 0, 0, 0});
    else
      Add({0x64, 0xa1, 0, 0, 0, 0});
    LinkStore = Text.Data.size();
    Add({0x89, uint8_t(0x45 | LinkRegister << 3), 0xe8});
    if (!EarlyNode)
      AddNodeAddress();
    Publication = Text.Data.size();
    if (NodeRegister == 0)
      Add({0x64, 0xa3, 0, 0, 0, 0});
    else
      Add({0x64, 0x89, uint8_t(5 | NodeRegister << 3), 0, 0, 0, 0});
    Text.Size = Text.FileSz = Text.Data.size();
    Site.Range = {Text.VA, Text.VA + Text.Size};
    Site.HandlerVA = 0x402000;
    Image.Segments.push_back(std::move(Text));
  }

  bool prove() {
    RegistrationChainInfo Chain;
    const bool Result = proveFixedCxxRegistrationLayout(Image, Site, Chain);
    if (Result) {
      EXPECT_EQ(Chain.RegistrationOffset, -24);
      EXPECT_EQ(Chain.TryLevelOffset, -16);
      EXPECT_EQ(Chain.ChainInstallVA, Site.Range.Begin + Publication);
    } else {
      EXPECT_FALSE(Chain.RegistrationOffset);
      EXPECT_FALSE(Chain.TryLevelOffset);
      EXPECT_FALSE(Chain.ChainInstallVA);
    }
    return Result;
  }
};

TEST(RegistrationEntryLayout, AcceptsBoundedArgumentsAndBothPublicationOrders) {
  const std::vector<std::vector<uint8_t>> Arguments = {
      {},
      {0x8b, 0x45, 8, 0x8b, 0x45, 12},
      {0x8b, 0x7d, 12, 0x8b, 0x75, 8},
      {0x8b, 0x85, 0x80, 0, 0, 0},
      {0x66, 0x8b, 0x45, 12, 0x8a, 0x45, 8},
      {0x8a, 0x65, 8, 0x8a, 0x6d, 12}, // AH/CH preserve ESP and EBP.
      {0x88, 0xc8, 0x66, 0x89, 0xd0},  // CL and DX copied into AL/AX.
      {0x8a, 0xe1, 0x66, 0x8b, 0xda},  // CL/DX copied into AH/BX.
      {0x89, 0xcf, 0x8b, 0xf2},        // ECX/EDX copied into EDI/ESI.
      {0x89, 0x4d, 0xe0, 0x89, 0x55, 0xdc},
      {0x88, 0x4d, 0xe3, 0x66, 0x89, 0x55, 0xe1},
      {0x89, 0x8d, 0xd8, 0xff, 0xff, 0xff}};
  for (const auto &Prefix : Arguments)
    for (bool Early : {false, true})
      for (unsigned Register : {1u, 2u, 3u, 6u, 7u}) {
        EntryLayoutImage F(Prefix, Early, Register);
        EXPECT_TRUE(F.prove());
      }
}

TEST(RegistrationEntryLayout, RejectsFrameChangesAndRuntimeCellAliases) {
  const std::vector<std::vector<uint8_t>> Arguments = {
      {0x8b, 0x65, 8},       // Incoming memory cannot replace ESP.
      {0x8b, 0x6d, 8},       // Or replace EBP.
      {0x66, 0x8b, 0x65, 8}, // SP still changes ESP.
      {0x66, 0x8b, 0x6d, 8}, // BP still changes EBP.
      {0x89, 0xcc},          // Register copies must also preserve ESP/EBP.
      {0x8b, 0xea},
      {0x66, 0x89, 0xd4},
      {0x66, 0x8b, 0xe9},
      {0x8b, 0x45, 4}, // The caller PC is not an argument word.
      {0x8a, 0x45, 7}, // Nor is its last byte.
      {0x66, 0x8b, 0x45, 6},
      {0x89, 0x4d, 0xe4},       // SavedESP.
      {0x88, 0x4d, 0xe4},       // A partial SavedESP write is also forbidden.
      {0x66, 0x89, 0x55, 0xe3}, // A word crossing into SavedESP.
      {0x89, 0x4d, 0xe8},       // Next link.
      {0x89, 0x4d, 0xec},       // Personality.
      {0x89, 0x4d, 0xf0},       // State.
      {0x89, 0x4d, 0xc0},       // Outside the allocated private frame.
      {0x90},                   // No guessed instruction semantics.
      {0x66, 0x8a, 0x45, 8},    // No redundant or repeated prefixes.
      {0x66, 0x66, 0x8b, 0x45, 8},
      {0x8b, 0x45}, // Truncated argument load.
      {0x66, 0x8b, 0x45},
      {0x8a, 0x45},
      {0x89, 0x8d, 0xff, 0, 0, 0}};
  for (const auto &Prefix : Arguments)
    for (bool Early : {false, true}) {
      EntryLayoutImage F(Prefix, Early);
      EXPECT_FALSE(F.prove());
    }
  for (unsigned Register : {0u, 4u, 5u}) {
    EntryLayoutImage F({}, true, Register);
    EXPECT_FALSE(F.prove());
  }
  for (bool Early : {false, true}) {
    EntryLayoutImage F({}, Early);
    ++F.Site.InstallVA;
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationEntryLayout, TracksSavedSPNodeAndPreviousLinkRegisters) {
  for (bool Early : {false, true})
    for (unsigned Node : {0u, 1u, 2u, 3u, 6u, 7u})
      for (unsigned Link : {0u, 1u, 2u, 3u, 6u, 7u})
        for (unsigned SavedSP : {0u, 1u, 2u, 3u, 4u, 6u, 7u}) {
          EntryLayoutImage F({}, Early, Link, Node, SavedSP);
          EXPECT_EQ(F.prove(), !Early || Node != Link)
              << "early=" << Early << " node=" << Node << " link=" << Link
              << " saved-sp=" << SavedSP;
        }
}

TEST(RegistrationEntryLayout, RejectsMismatchedOrTruncatedRegisterDefinitions) {
  for (bool Early : {false, true}) {
    for (unsigned Register : {4u, 5u}) {
      EntryLayoutImage Node({}, Early, 0, Register);
      EXPECT_FALSE(Node.prove());
      EntryLayoutImage Link({}, Early, Register);
      EXPECT_FALSE(Link.prove());
    }
    EntryLayoutImage SavedBP({}, Early, 2, 1, 5);
    EXPECT_FALSE(SavedBP.prove());
    for (unsigned Node : {0u, 1u, 2u, 3u, 6u, 7u}) {
      const unsigned Link = Node == 0 ? 1 : 0;
      EntryLayoutImage SavedSP({}, Early, Link, Node, 2);
      SavedSP.Image.Segments[0].Data[SavedSP.SavedSPStore + 1] ^= 8;
      EXPECT_FALSE(SavedSP.prove());
      EntryLayoutImage Previous({}, Early, Link, Node);
      Previous.Image.Segments[0].Data[Previous.LinkStore + 1] ^= 8;
      EXPECT_FALSE(Previous.prove());
      EntryLayoutImage Published({}, Early, Link, Node);
      // A different LEA destination cannot justify the published register.
      Published.Image.Segments[0].Data[Published.NodeAddress + 1] ^= 8;
      EXPECT_FALSE(Published.prove());
      EntryLayoutImage Truncated({}, Early, Link, Node);
      auto &Text = Truncated.Image.Segments[0];
      Text.Data.pop_back();
      Text.Size = Text.FileSz = Text.Data.size();
      --Truncated.Site.Range.End;
      EXPECT_FALSE(Truncated.prove());
    }
  }
}
} // namespace
