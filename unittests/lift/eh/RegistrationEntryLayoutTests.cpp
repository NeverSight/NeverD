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

  EntryLayoutImage(llvm::ArrayRef<uint8_t> Arguments, bool EarlyNode,
                   unsigned LinkRegister = 1) {
    Segment Text;
    Text.VA = 0x401000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {0x55, 0x89, 0xe5, 0x53, 0x57, 0x56, 0x83, 0xec, 0x30};
    auto Add = [&](llvm::ArrayRef<uint8_t> Bytes) {
      Text.Data.insert(Text.Data.end(), Bytes.begin(), Bytes.end());
    };
    Add(Arguments);
    Add({0x89, 0x65, 0xe4, 0xc7, 0x45, 0xf0, 0xff, 0xff, 0xff, 0xff});
    if (EarlyNode)
      Add({0x8d, 0x45, 0xe8});
    Add({0xc7, 0x45, 0xec, 0x00, 0x20, 0x40, 0x00});
    Site.InstallVA = Text.VA + Text.Data.size();
    if (EarlyNode)
      Add({0x64, 0x8b, uint8_t(5 | LinkRegister << 3), 0, 0, 0, 0});
    else {
      Add({0x64, 0xa1, 0, 0, 0, 0});
      LinkRegister = 0;
    }
    Add({0x89, uint8_t(0x45 | LinkRegister << 3), 0xe8});
    if (!EarlyNode)
      Add({0x8d, 0x45, 0xe8});
    Add({0x64, 0xa3, 0, 0, 0, 0});
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
      EXPECT_EQ(Chain.ChainInstallVA, Site.Range.End - 6);
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
      {0x89, 0x4d, 0xe0, 0x89, 0x55, 0xdc},
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
      {0x8b, 0x65, 8},    // Incoming memory cannot replace ESP.
      {0x8b, 0x6d, 8},    // Or replace EBP.
      {0x8b, 0x45, 4},    // The caller PC is not an argument word.
      {0x89, 0x4d, 0xe4}, // SavedESP.
      {0x89, 0x4d, 0xe8}, // Next link.
      {0x89, 0x4d, 0xec}, // Personality.
      {0x89, 0x4d, 0xf0}, // State.
      {0x89, 0x4d, 0xc0}, // Outside the allocated private frame.
      {0x90},             // No guessed instruction semantics.
      {0x8b, 0x45},       // Truncated argument load.
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
} // namespace
