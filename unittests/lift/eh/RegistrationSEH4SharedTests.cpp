//===- RegistrationSEH4SharedTests.cpp - Shared EH4 frame ownership -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise physical SEH4 entry and exit calls through ordinary CFG analysis.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

using namespace neverd;

namespace {
BinaryImage sharedSEH4Image(uint32_t Base, bool Wide,
                            uint32_t Allocation = 32) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.DynInfo.SecurityCookieRVA = 0x2080;
  for (bool Code : {true, false}) {
    Segment Segment;
    Segment.VA = Base + (Code ? 0x1000 : 0x2000);
    Segment.Size = Segment.FileSz = 0x400;
    Segment.Flags = SegmentFlags::Readable |
                    (Code ? SegmentFlags::Executable : SegmentFlags::None);
    Segment.Data.resize(Segment.Size, Code ? 0xcc : 0);
    Section Section;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.Flags = Segment.Flags;
    Image.Segments.push_back(std::move(Segment));
    Image.Sections.push_back(std::move(Section));
  }
  auto &Text = Image.Segments.front().Data;
  auto Put = [&](size_t At, std::initializer_list<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(), Text.begin() + At);
  };
  auto Word = [&](size_t At, uint32_t Value) {
    writeLE<uint32_t>(Text.data() + At, Value);
  };
  auto Call = [&](size_t At, size_t Target) {
    Text[At] = 0xe8;
    Word(At + 1, uint32_t(Target - At - 5));
  };
  Text[0] = Wide ? 0x68 : 0x6a;
  if (Wide)
    Word(1, Allocation);
  else
    Text[1] = uint8_t(Allocation);
  const size_t Push = Wide ? 5 : 2;
  Text[Push] = 0x68;
  Word(Push + 1, Base + 0x2100);
  Call(Push + 5, 0x100);
  const size_t Body = Push + 10;
  Put(Body, {0xc7, 0x45, 0xfc, 0, 0, 0, 0, 0x90, 0xc7, 0x45, 0xfc, 0xfe, 0xff,
             0xff, 0xff, 0x31, 0xc0});
  Call(Body + 17, 0x150);
  Text[Body + 22] = 0xc3;
  Put(0x80, {0xb8, 1, 0, 0, 0, 0xc3});

  // Save the caller EBP in its allocation word, move the helper return PC,
  // encode the table and publish the registration only after initialization.
  Put(0x100,
      {0x68, 0,    0,    0,    0,    0x64, 0xff, 0x35, 0,    0,    0,    0,
       0x8b, 0x44, 0x24, 0x10, 0x89, 0x6c, 0x24, 0x10, 0x8d, 0x6c, 0x24, 0x10,
       0x2b, 0xe0, 0x53, 0x56, 0x57, 0xa1, 0,    0,    0,    0,    0x31, 0x45,
       0xfc, 0x33, 0xc5, 0x50, 0x89, 0x65, 0xe8, 0xff, 0x75, 0xf8, 0x8b, 0x45,
       0xfc, 0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0x89, 0x45, 0xf8, 0x8d,
       0x45, 0xf0, 0x64, 0xa3, 0,    0,    0,    0,    0xc3});
  Word(0x101, Base + 0x1200);
  Word(0x11e, Base + 0x2080);
  Put(0x150, {0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0,    0,    0,    0,
              0x59, 0x5f, 0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0x51, 0xc3});

  Put(0x200, {0x55, 0x8b, 0xec, 0xff, 0x75, 0x14, 0xff, 0x75, 0x10,
              0xff, 0x75, 0x0c, 0xff, 0x75, 0x08, 0x68, 0,    0,
              0,    0,    0x68, 0,    0,    0,    0});
  Word(0x210, Base + 0x1280);
  Word(0x215, Base + 0x2080);
  Call(0x219, 0x290);
  Put(0x21e, {0x83, 0xc4, 0x18, 0x5d, 0xc3});
  Text[0x280] = 0xc3;
  Put(0x290, {0xff, 0x25, 0, 0, 0, 0});
  Word(0x292, Base + 0x2008);
  Image.Imports.push_back(
      {"msvcr100.dll", "_except_handler4_common", 0, Base + 0x2008});
  auto &Data = Image.Segments.back().Data;
  const uint32_t Words[] = {
      uint32_t(-2), 0, uint32_t(-32 - Allocation), 0, uint32_t(-2), 0,
      Base + 0x1080};
  for (size_t I = 0; I != std::size(Words); ++I)
    writeLE<uint32_t>(Data.data() + 0x100 + I * 4, Words[I]);
  for (uint32_t RVA : {0x1000, 0x1080, 0x1100, 0x1150, 0x1200, 0x1280, 0x1290})
    Image.Symbols.push_back(Symbol::makeFunc(Base + RVA));
  coff_loader::parseX86RegistrationExceptions(Image);
  return Image;
}

LowFunc liftSharedSEH4(BinaryImage &Image) {
  Decoder Decoder;
  EXPECT_TRUE(Decoder.init(Image));
  return CFGBuilder().build(Image, Decoder, Image.Base + 0x1000, "shared-seh4");
}
} // namespace

TEST(RegistrationSEH4Shared, OrdinaryAnalysisOwnsTheInstalledFrame) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Wide : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Wide);
      auto Image = sharedSEH4Image(Base, Wide);
      const auto *EH = Image.ExceptionMetadata.findFunction(Base + 0x1000);
      ASSERT_NE(EH, nullptr);
      ASSERT_TRUE(EH->Registration);
      ASSERT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
      EXPECT_EQ(EH->Registration->RegistrationOffset, -16);
      EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
      auto Low = liftSharedSEH4(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      ASSERT_TRUE(Low.RegistrationStates);
      const auto &States = *Low.RegistrationStates;
      std::string Diagnostics;
      for (const auto &Diagnostic : States.Diagnostics)
        Diagnostics += Diagnostic + "\n";
      SCOPED_TRACE(Diagnostics);
      EXPECT_TRUE(States.Complete);
      EXPECT_TRUE(States.CallbackStatesComplete);
      EXPECT_TRUE(States.RegistrationLifetimeComplete);
      EXPECT_TRUE(States.ChainOperationsComplete);
      EXPECT_TRUE(States.SecurityCookiesComplete);
      ASSERT_TRUE(States.SEH4Prologue);
      EXPECT_EQ(States.SEH4Prologue->HelperVA, Base + 0x1100);
      EXPECT_EQ(States.SEH4Prologue->StackOffset, -64);
      EXPECT_FALSE(States.CxxPrologue);
    }
}

TEST(RegistrationSEH4Shared, RequiresEveryCurrentHelperInstruction) {
  const auto Original = sharedSEH4Image(0x400000, false);
  const auto *EH = Original.ExceptionMetadata.findFunction(0x401000);
  ASSERT_NE(EH, nullptr);
  ASSERT_TRUE(coff_loader::getCheckedX86SEH4PrologueCall(Original, *EH));
  ASSERT_TRUE(
      coff_loader::getCheckedX86SEH4EpilogueCall(Original, *EH, 0x40101d));
  for (size_t Byte = 0; Byte != 69; ++Byte) {
    auto Changed = Original;
    Changed.Segments.front().Data[0x100 + Byte] ^= 1;
    EXPECT_FALSE(coff_loader::getCheckedX86SEH4PrologueCall(Changed, *EH))
        << Byte;
  }
  for (size_t Byte = 0; Byte != 20; ++Byte) {
    auto Changed = Original;
    Changed.Segments.front().Data[0x150 + Byte] ^= 1;
    EXPECT_FALSE(
        coff_loader::getCheckedX86SEH4EpilogueCall(Changed, *EH, 0x40101d))
        << Byte;
  }
  for (size_t Byte = 0; Byte != 12; ++Byte) {
    if (Byte == 1)
      continue; // A changed allocation is a different valid protocol input.
    auto Changed = Original;
    Changed.Segments.front().Data[Byte] ^= 1;
    EXPECT_FALSE(coff_loader::getCheckedX86SEH4PrologueCall(Changed, *EH))
        << Byte;
  }
}

TEST(RegistrationSEH4Shared, PrefixEvidenceCannotSurviveChangedLowIR) {
  auto Image = sharedSEH4Image(0x400000, false);
  const auto Original = liftSharedSEH4(Image);
  ASSERT_TRUE(getCheckedX86RegistrationSEH4ProloguePrefix(Image, Original));
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Original;
    auto &Entry = Changed.Blocks.front();
    if (Mutation == 0)
      Entry.Ops.front().Opcode = NdOp::INT_XOR;
    if (Mutation == 1)
      Entry.Ops.back().Inputs[0].Offset += 1;
    if (Mutation == 2)
      Entry.InstructionBoundaries.back().Immediate.reset();
    if (Mutation == 3)
      Changed.OrdinaryModuleAnalysisRoots.insert(Image.Base + 0x1002);
    if (Mutation == 4)
      Entry.Preds.push_back(Entry.Id);
    EXPECT_FALSE(getCheckedX86RegistrationSEH4ProloguePrefix(Image, Changed))
        << Mutation;
  }
}

TEST(RegistrationSEH4Shared, SavedStackWordCannotReplaceANonvolatileSave) {
  for (uint32_t Allocation : {0, 1, 4, 32, 36, 200}) {
    auto Image = sharedSEH4Image(0x400000, true, Allocation);
    auto Low = liftSharedSEH4(Image);
    ASSERT_TRUE(Low.RegistrationStates);
    // Small allocations overlap the helper's mandatory [EBP-24] ESP store
    // with the saved ESI/EBX bytes. Exact instructions alone cannot restore
    // those nonvolatile registers at the physical epilogue.
    EXPECT_EQ(Low.RegistrationStates->Complete, Allocation >= 32) << Allocation;
    EXPECT_EQ(Low.RegistrationStates->ChainOperationsComplete, Allocation >= 32)
        << Allocation;
  }
}

TEST(RegistrationSEH4Shared, WrapperBindsBothCallFormsAndHotpatchEntry) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Hotpatch : {false, true})
      for (bool Direct : {false, true}) {
        auto Image = sharedSEH4Image(Base, false);
        auto &Text = Image.Segments.front().Data;
        std::vector<uint8_t> Body;
        if (Hotpatch)
          Body = {0x8b, 0xff};
        Body.insert(Body.end(), Text.begin() + 0x200, Text.begin() + 0x219);
        const size_t Operand = Body.size() + (Direct ? 1 : 2);
        Body.push_back(Direct ? 0xe8 : 0xff);
        if (!Direct)
          Body.push_back(0x15);
        Body.resize(Body.size() + 4);
        writeLE<uint32_t>(Body.data() + Operand,
                          Direct ? uint32_t(0x90 - Body.size())
                                 : Base + 0x2008);
        Body.insert(Body.end(), {0x83, 0xc4, 0x18, 0x5d, 0xc3});
        std::fill(Text.begin() + 0x200, Text.begin() + 0x240, 0xcc);
        std::copy(Body.begin(), Body.end(), Text.begin() + 0x200);
        for (const char *Module : {"msvcrt.dll", "msvcr100.dll", "ucrtbase.dll",
                                   "vcruntime140.dll", "vcruntime140d.dll"}) {
          Image.Imports.front().Module = Module;
          EXPECT_EQ(
              coff_loader::getCheckedX86EH4CookieCheck(Image, Base + 0x1200),
              Base + 0x1280)
              << Hotpatch << Direct << Module;
        }
      }
}

TEST(RegistrationSEH4Shared, WrapperRequiresExactArgumentsAndRuntimeBinding) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    auto Image = sharedSEH4Image(0x400000, false);
    auto &Text = Image.Segments.front().Data;
    if (Mutation == 0)
      Image.Imports.front().Module = "other.dll";
    if (Mutation == 1)
      Image.Imports.front().Name = "different_handler";
    if (Mutation == 2)
      Image.Imports.front().IsDelayImport = true;
    if (Mutation == 3)
      Image.Imports.push_back(Image.Imports.front());
    if (Mutation == 4)
      Image.Segments.front().Flags = Image.Sections.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Executable |
          SegmentFlags::Writable;
    if (Mutation == 5)
      Text[0x205] = 0x10;
    if (Mutation == 6)
      Text[0x215] ^= 4;
    if (Mutation == 7)
      Text[0x220] = 20;
    if (Mutation == 8) {
      Text[0x290] = 0xe9;
      writeLE<uint32_t>(Text.data() + 0x291, uint32_t(-5));
    }
    if (Mutation == 9)
      Image.Arch = Arch::X64;
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401200))
        << Mutation;
  }
}
