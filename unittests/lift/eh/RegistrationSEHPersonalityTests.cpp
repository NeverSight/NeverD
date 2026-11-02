//===- RegistrationSEHPersonalityTests.cpp - EH3 provider binding -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Check retained SEH3 veneers and their exact runtime import dependencies.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"

#include <cstdint>
#include <map>
#include <utility>

using namespace neverd;

TEST(RegistrationSEHPersonality, RequiresTheRuntimeImportAndExecutableVeneer) {
  for (const char *Provider : {"msvcrt.dll", "ucrtbase.dll", "VCRUNTIME140.dll",
                               "vcruntime140d.dll"}) {
    SCOPED_TRACE(Provider);
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Mutation == 6 ? Arch::ARM : Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = 0x400000;
      Segment Text;
      Text.VA = 0x401000;
      Text.Size = 6;
      Text.Flags = SegmentFlags::Readable;
      if (Mutation != 4)
        Text.Flags = Text.Flags | SegmentFlags::Executable;
      Text.Data = {0xff, uint8_t(Mutation == 3 ? 0x15 : 0x25), 0, 0, 0, 0};
      llvm::support::endian::write32le(Text.Data.data() + 2, 0x403000);
      if (Mutation == 5)
        Text.Data.pop_back();
      Image.Segments.push_back(std::move(Text));
      Segment Data;
      Data.VA = 0x403000;
      Data.Size = 4;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(4);
      Image.Segments.push_back(std::move(Data));
      Image.Imports.push_back(
          {Mutation == 1 ? "custom.dll" : Provider,
           Mutation == 2 ? "_except_handler4" : "_except_handler3", 0,
           0x403000});
      EXPECT_EQ(coff_loader::isCheckedX86SEH3Personality(Image, 0x401000),
                Mutation == 0);
    }
  }
}

TEST(RegistrationSEHPersonality, RetainedDependenciesRequireImmutableIdentity) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mode = 0; Mode != 17; ++Mode) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Mode);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = Base;
      for (bool Code : {true, false}) {
        Segment Segment;
        Segment.VA = Base + (Code ? 0x1000 : 0x2000);
        Segment.Size = Segment.FileSz = 32;
        Segment.Flags =
            SegmentFlags::Readable |
            (Code ? SegmentFlags::Executable : SegmentFlags::Writable);
        Segment.Data.resize(32, Code ? 0xcc : 0);
        Section Section;
        Section.VA = Segment.VA;
        Section.Size = Section.FileSz = Segment.Size;
        Section.Flags = Segment.Flags;
        Image.Sections.push_back(Section);
        Image.Segments.push_back(std::move(Segment));
      }
      auto &Code = Image.Segments.front().Data;
      Code[0] = 0xe9;
      llvm::support::endian::write32le(Code.data() + 1, 11);
      Code[16] = 0xff;
      Code[17] = 0x25;
      llvm::support::endian::write32le(Code.data() + 18, Base + 0x2000);
      Image.Imports.push_back(
          {"vcruntime140.dll", "_except_handler3", 0, Base + 0x2000});
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Base + 0x2000, "_except_handler3", 0,
          ImportStorageEvidence::ImportDirectory));
      Image.BaseRelocations.push_back(
          {Base + 0x1012, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      if (Mode == 1)
        Image.Imports.push_back(Image.Imports.front());
      if (Mode == 2)
        Image.Imports.front().IsDelayImport = true;
      if (Mode == 3)
        Image.Imports.front().Module = "custom.dll";
      if (Mode == 4)
        Image.Imports.front().Name = "_except_handler4_common";
      if (Mode == 5)
        Image.Sections.front().Flags =
            Image.Sections.front().Flags | SegmentFlags::Writable;
      if (Mode == 6)
        Image.Segments.front().Flags =
            Image.Segments.front().Flags | SegmentFlags::Writable;
      if (Mode == 7)
        ++Image.BaseRelocations.front().Address;
      if (Mode == 8)
        Image.BaseRelocations.push_back(Image.BaseRelocations.front());
      if (Mode == 9)
        Image.BaseRelocations.push_back(
            {Base + 0x1001, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      if (Mode == 10)
        llvm::support::endian::write32le(Code.data() + 1, uint32_t(-5));
      if (Mode == 11)
        Image.Sections.push_back(Image.Sections.front());
      if (Mode == 12)
        Image.Bits = Bitness::Bits64;
      if (Mode == 13)
        Image.IsRelocatable = true;
      if (Mode == 14)
        Image.Segments.front().FileSz = 21;
      if (Mode == 15)
        Image.ConflictingImportStorageSlots.insert(Base + 0x2000);
      if (Mode == 16) {
        // A callable import veneer is not the data slot dereferenced by FF25.
        Image.Symbols.push_back(Symbol::makeFunc(Base + 0x1010));
        ASSERT_TRUE(Image.recordImportStub(Base + 0x1010, 0));
        llvm::support::endian::write32le(Code.data() + 18, Base + 0x1010);
        ASSERT_NE(Image.findImportAt(Base + 0x1010), nullptr);
      }
      const auto ABI =
          coff_loader::getCheckedX86SEH3PersonalityABI(Image, Base + 0x1000);
      ASSERT_EQ(bool(ABI), Mode == 0);
      if (!ABI)
        continue;
      EXPECT_EQ(ABI->IATVA, Base + 0x2000);
      EXPECT_EQ(ABI->SecurityCookieVA, 0u);
      EXPECT_EQ(ABI->CookieCheckVA, 0u);
      EXPECT_TRUE(ABI->ImageReads.empty());
      EXPECT_EQ(ABI->AbsoluteOperands,
                (std::map<va_t, va_t>{{Base + 0x1012, Base + 0x2000}}));
      ASSERT_EQ(ABI->CodeRanges.size(), 2u);
      EXPECT_EQ(ABI->CodeRanges[0].Begin, Base + 0x1000);
      EXPECT_EQ(ABI->CodeRanges[0].End, Base + 0x1005);
      EXPECT_EQ(ABI->CodeRanges[1].Begin, Base + 0x1010);
      EXPECT_EQ(ABI->CodeRanges[1].End, Base + 0x1016);
      ExceptionFunction Function;
      Function.ParseStatus = ExceptionParseStatus::Complete;
      Function.Personality = ExceptionPersonality::ExceptHandler3;
      Function.PersonalityVA = Base + 0x1000;
      Function.Registration.emplace();
      Function.Registration->HandlerVA = Function.PersonalityVA;
      size_t Work = 0;
      const auto Runtime =
          coff_loader::getCheckedX86SEHRuntimeCode(Image, Function, Work);
      ASSERT_TRUE(Runtime);
      EXPECT_EQ(Runtime->AbsoluteOperands, ABI->AbsoluteOperands);
      EXPECT_EQ(Runtime->Code.size(), 2u);
      EXPECT_EQ(Runtime->IATVA, Base + 0x2000);
      EXPECT_EQ(Runtime->ImportName, "_except_handler3");
      Image.BaseRelocations.clear();
      Work = 0;
      EXPECT_FALSE(
          coff_loader::getCheckedX86SEHRuntimeCode(Image, Function, Work));
    }
}
