//===- MachOI386RelocationFixtureTests.cpp - Mach-O i386 real fixture shape tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "MachOI386RelocationTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::macho_loader::detail;
using namespace neverd::macho_i386_test;

TEST_F(MachOI386Relocation, RealFixturesContainRequiredRawRelocationShapes) {
  auto PICBytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto NoPICBytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  ASSERT_FALSE(PICBytes.empty());
  ASSERT_FALSE(NoPICBytes.empty());
  auto PIC = createMachOObject(PICBytes);
  auto NoPIC = createMachOObject(NoPICBytes);
  ASSERT_NE(PIC, nullptr);
  ASSERT_NE(NoPIC, nullptr);
  EXPECT_FALSE(PIC->is64Bit());
  EXPECT_EQ(PIC->getHeader().cputype, llvm::MachO::CPU_TYPE_I386);
  EXPECT_EQ(PIC->getHeader().filetype, llvm::MachO::MH_OBJECT);

  auto PICText = findSection(*PIC, section_names::macho::Text);
  ASSERT_TRUE(PICText.has_value());
  auto PICRelocs = relocations(*PICText);
  size_t DifferencePairs = 0;
  for (size_t I = 0; I < PICRelocs.size(); ++I) {
    auto Info = PIC->getRelocation(PICRelocs[I].getRawDataRefImpl());
    uint32_t Type = PIC->getAnyRelocationType(Info);
    if (Type != llvm::MachO::GENERIC_RELOC_SECTDIFF &&
        Type != llvm::MachO::GENERIC_RELOC_LOCAL_SECTDIFF)
      continue;
    ASSERT_LT(I + 1, PICRelocs.size());
    auto PairInfo = PIC->getRelocation(PICRelocs[I + 1].getRawDataRefImpl());
    EXPECT_TRUE(PIC->isRelocationScattered(Info));
    EXPECT_TRUE(PIC->isRelocationScattered(PairInfo));
    EXPECT_EQ(PIC->getAnyRelocationType(PairInfo),
              llvm::MachO::GENERIC_RELOC_PAIR);
    EXPECT_EQ(PIC->getAnyRelocationLength(Info), 2u);
    EXPECT_FALSE(PIC->getAnyRelocationPCRel(Info));
    ++DifferencePairs;
    ++I;
  }
  EXPECT_GE(DifferencePairs, 3u);

  auto NoPICText = findSection(*NoPIC, section_names::macho::Text);
  ASSERT_TRUE(NoPICText.has_value());
  size_t LocalVanilla = 0;
  for (const auto &Reloc : relocations(*NoPICText)) {
    auto Info = NoPIC->getRelocation(Reloc.getRawDataRefImpl());
    if (NoPIC->getAnyRelocationType(Info) ==
            llvm::MachO::GENERIC_RELOC_VANILLA &&
        !NoPIC->isRelocationScattered(Info) &&
        !NoPIC->getPlainRelocationExternal(Info) &&
        !NoPIC->getAnyRelocationPCRel(Info) &&
        NoPIC->getAnyRelocationLength(Info) == 2u)
      ++LocalVanilla;
  }
  EXPECT_GE(LocalVanilla, 3u);

  for (const auto *Obj : {PIC.get(), NoPIC.get()}) {
    auto Data = findSection(*Obj, section_names::macho::Data);
    ASSERT_TRUE(Data.has_value());
    auto DataRelocs = relocations(*Data);
    ASSERT_GE(DataRelocs.size(), 2u);
    for (const auto &Reloc : DataRelocs) {
      auto Info = Obj->getRelocation(Reloc.getRawDataRefImpl());
      EXPECT_EQ(Obj->getAnyRelocationType(Info),
                llvm::MachO::GENERIC_RELOC_VANILLA);
      EXPECT_FALSE(Obj->isRelocationScattered(Info));
      EXPECT_FALSE(Obj->getPlainRelocationExternal(Info));
      EXPECT_FALSE(Obj->getAnyRelocationPCRel(Info));
      EXPECT_EQ(Obj->getAnyRelocationLength(Info), 2u);
    }
  }
}

TEST_F(MachOI386Relocation,
       ObjectSectionsProjectToExactNonOverlappingSegments) {
  for (llvm::StringRef Name : {llvm::StringRef("test_macho_i386.o"),
                               llvm::StringRef("test_macho_i386_nopic.o")}) {
    SCOPED_TRACE(Name.str());
    auto ImgOrErr = loadBinary(fixture(Name));
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;

    size_t NonEmptySections =
        std::count_if(Img.Sections.begin(), Img.Sections.end(),
                      [](const Section &Sec) { return Sec.Size != 0; });
    ASSERT_EQ(Img.Segments.size(), NonEmptySections);
    for (const Section &Sec : Img.Sections) {
      if (Sec.Size == 0)
        continue;
      auto It = std::find_if(Img.Segments.begin(), Img.Segments.end(),
                             [&](const Segment &Seg) {
                               return Seg.VA == Sec.VA && Seg.Size == Sec.Size;
                             });
      ASSERT_NE(It, Img.Segments.end()) << Sec.SegmentName << ',' << Sec.Name;
      EXPECT_EQ(It->Name, Sec.SegmentName);
      EXPECT_EQ(It->FileOff, Sec.FileOff);
      EXPECT_EQ(It->FileSz, Sec.FileSz);
      EXPECT_EQ(It->Flags, Sec.Flags);
      EXPECT_EQ(It->Data, Sec.Data);
    }

    const Section *Text = Img.getSectionByName(section_names::macho::Text);
    const Section *Const = Img.getSectionByName(section_names::macho::Const);
    const Segment *TextSegment = Img.getTextSegment();
    ASSERT_NE(Text, nullptr);
    ASSERT_NE(Const, nullptr);
    ASSERT_NE(TextSegment, nullptr);
    EXPECT_TRUE(Text->isExecutable());
    EXPECT_FALSE(Const->isExecutable());
    EXPECT_EQ(TextSegment->VA, Text->VA);
    EXPECT_EQ(TextSegment->Size, Text->Size);

    for (size_t I = 0; I < Img.Segments.size(); ++I) {
      const Segment &A = Img.Segments[I];
      ASSERT_LE(A.Size, InvalidVA - A.VA);
      for (size_t J = I + 1; J < Img.Segments.size(); ++J) {
        const Segment &B = Img.Segments[J];
        ASSERT_LE(B.Size, InvalidVA - B.VA);
        EXPECT_TRUE(A.VA + A.Size <= B.VA || B.VA + B.Size <= A.VA)
            << A.Name << " overlaps " << B.Name;
      }
    }

    const Section *Bss = Img.getSectionByName(section_names::macho::Bss);
    ASSERT_NE(Bss, nullptr);
    EXPECT_EQ(Bss->FileSz, 0u);
    ASSERT_EQ(Bss->Data.size(), Bss->Size);
    EXPECT_TRUE(std::all_of(Bss->Data.begin(), Bss->Data.end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST_F(MachOI386Relocation, OverlappingObjectSectionsReturnControlledError) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto DataHeader = rawSectionHeaderOffset(Bytes, section_names::macho::Data);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(DataHeader.has_value());
  size_t AddressOffset = *DataHeader + offsetof(llvm::MachO::section, addr);
  ASSERT_TRUE(rangeInBounds(AddressOffset, sizeof(uint32_t), Bytes.size()));
  writeLE<uint32_t>(Bytes.data() + AddressOffset, Text->Address);

  auto ImgOrErr = loadBinary(writeMutation("overlapping_sections.o", Bytes));
  ASSERT_FALSE(static_cast<bool>(ImgOrErr));
  std::string Error = llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(Error.find("relocatable sections overlap"), std::string::npos)
      << Error;
}

TEST_F(MachOI386Relocation,
       RealFixturesUseExactFormulasAndSectionAwareProvenance) {
  for (llvm::StringRef Name : {llvm::StringRef("test_macho_i386.o"),
                               llvm::StringRef("test_macho_i386_nopic.o")}) {
    SCOPED_TRACE(Name.str());
    auto Bytes = readBinaryFile(fixture(Name));
    ASSERT_FALSE(Bytes.empty());
    auto Obj = createMachOObject(Bytes);
    ASSERT_NE(Obj, nullptr);

    auto ImgOrErr = loadBinary(fixture(Name));
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Sections.size(),
              static_cast<size_t>(
                  std::distance(Obj->section_begin(), Obj->section_end())));

    size_t SecIndex = 0;
    for (const llvm::object::SectionRef &Sec : Obj->sections()) {
      SCOPED_TRACE(SecIndex);
      auto ContentsOrErr = Sec.getContents();
      ASSERT_TRUE(static_cast<bool>(ContentsOrErr))
          << llvm::toString(ContentsOrErr.takeError());
      llvm::StringRef Contents = *ContentsOrErr;
      llvm::ArrayRef<uint8_t> RawData(
          reinterpret_cast<const uint8_t *>(Contents.data()), Contents.size());
      auto Relocs = relocations(Sec);

      for (size_t I = 0; I < Relocs.size(); ++I) {
        auto Info = Obj->getRelocation(Relocs[I].getRawDataRefImpl());
        uint32_t Type = Obj->getAnyRelocationType(Info);
        uint32_t Length = Obj->getAnyRelocationLength(Info);
        ASSERT_LE(Length, 2u);
        uint8_t Width = uint8_t(1u << Length);
        uint64_t Offset = Relocs[I].getOffset();
        va_t Place = Img.Sections[SecIndex].VA + Offset;

        if (Type == llvm::MachO::GENERIC_RELOC_VANILLA) {
          bool IsPCRel = Obj->getAnyRelocationPCRel(Info);
          auto Existing = readRelocationField(RawData, Offset, Width, IsPCRel);
          ASSERT_TRUE(Existing.has_value());

          int64_t Target = 0;
          int64_t Addend = *Existing;
          if (Obj->isRelocationScattered(Info)) {
            uint64_t EncodedTarget = Obj->getScatteredRelocationValue(Info);
            auto TargetSec = findOriginalSection(*Obj, EncodedTarget);
            ASSERT_TRUE(TargetSec.has_value());
            Target = Img.Sections[TargetSec->Index].VA;
            Addend -= TargetSec->Base;
          } else {
            ASSERT_FALSE(Obj->getPlainRelocationExternal(Info));
            uint32_t TargetSectionNumber =
                Obj->getPlainRelocationSymbolNum(Info);
            ASSERT_GT(TargetSectionNumber, 0u);
            ASSERT_LE(TargetSectionNumber, Img.Sections.size());
            size_t TargetIndex = TargetSectionNumber - 1;
            Target = Img.Sections[TargetIndex].VA;
            Addend -= Obj->getAnyRelocationSection(Info).getAddress();
            if (IsPCRel)
              Addend += Sec.getAddress() + Offset + Width;
          }

          auto Expected =
              evaluateI386Vanilla({Target, Addend, Place, Width, IsPCRel});
          ASSERT_TRUE(Expected.has_value());
          auto Actual = readLoadedField(Img, Place, Width, IsPCRel);
          ASSERT_TRUE(Actual.has_value());
          EXPECT_EQ(*Actual, *Expected);
          continue;
        }

        if (Type != llvm::MachO::GENERIC_RELOC_SECTDIFF &&
            Type != llvm::MachO::GENERIC_RELOC_LOCAL_SECTDIFF)
          continue;

        ASSERT_LT(I + 1, Relocs.size());
        auto PairInfo = Obj->getRelocation(Relocs[I + 1].getRawDataRefImpl());
        ASSERT_TRUE(Obj->isRelocationScattered(Info));
        ASSERT_TRUE(Obj->isRelocationScattered(PairInfo));
        ASSERT_EQ(Obj->getAnyRelocationType(PairInfo),
                  llvm::MachO::GENERIC_RELOC_PAIR);
        auto Existing = readRelocationField(RawData, Offset, Width, true);
        ASSERT_TRUE(Existing.has_value());

        uint64_t EncodedA = Obj->getScatteredRelocationValue(Info);
        uint64_t EncodedB = Obj->getScatteredRelocationValue(PairInfo);
        auto SectionA = findOriginalSection(*Obj, EncodedA);
        auto SectionB = findOriginalSection(*Obj, EncodedB);
        ASSERT_TRUE(SectionA.has_value());
        ASSERT_TRUE(SectionB.has_value());
        int64_t FinalA =
            Img.Sections[SectionA->Index].VA + (EncodedA - SectionA->Base);
        int64_t FinalB =
            Img.Sections[SectionB->Index].VA + (EncodedB - SectionB->Base);
        auto Expected = evaluateI386SectionDifference(FinalA, FinalB, EncodedA,
                                                      EncodedB, *Existing);
        ASSERT_TRUE(Expected.has_value());
        auto Actual = readLoadedField(Img, Place, Width, true);
        ASSERT_TRUE(Actual.has_value());
        EXPECT_EQ(*Actual, *Expected);
        ++I;
      }
      ++SecIndex;
    }

    const Section *Text = Img.getSectionByName(section_names::macho::Text);
    const Section *ReadOnly = Img.getSectionByName(section_names::macho::Const);
    const Section *Data = Img.getSectionByName(section_names::macho::Data);
    ASSERT_NE(Text, nullptr);
    ASSERT_NE(ReadOnly, nullptr);
    ASSERT_NE(Data, nullptr);
    EXPECT_TRUE(Text->isReadable());
    EXPECT_FALSE(Text->isWritable());
    EXPECT_TRUE(Text->isExecutable());
    EXPECT_TRUE(ReadOnly->isReadable());
    EXPECT_FALSE(ReadOnly->isWritable());
    EXPECT_FALSE(ReadOnly->isExecutable());
    EXPECT_TRUE(Data->isReadable());
    EXPECT_TRUE(Data->isWritable());
    EXPECT_FALSE(Data->isExecutable());
    EXPECT_NE(Img.CodePtrRelocSlots.count(Data->VA + 8), 0u);
    // A full-width code pointer stored in data is owned by its relocation
    // slot. Only a relocation embedded in an instruction publishes a bare
    // CodeRefTarget; otherwise the same occurrence would be modeled twice.
    EXPECT_EQ(Img.CodeRefTargets.count(Text->VA), 0u);

    const Symbol *ReadOnlyValue = findSymbol(Img, "_readonly_value");
    const Symbol *ReadOnlySlot = findSymbol(Img, "_i386_readonly_dispatch");
    ASSERT_NE(ReadOnlyValue, nullptr);
    ASSERT_NE(ReadOnlySlot, nullptr);
    EXPECT_NE(Img.RelocDataAddrs.count(ReadOnlyValue->Addr), 0u);
    EXPECT_NE(Img.DataPtrRelocSlots.count(ReadOnlySlot->Addr), 0u);
    EXPECT_EQ(Img.WritableRelocDataAddrs.count(ReadOnlyValue->Addr), 0u);
    EXPECT_EQ(Img.CodeRefTargets.count(ReadOnlyValue->Addr), 0u);
    EXPECT_EQ(Img.CodePtrRelocSlots.count(ReadOnlySlot->Addr), 0u);

    if (Name.ends_with("_nopic.o")) {
      for (llvm::StringRef SymbolName :
           {llvm::StringRef("_global_value"), llvm::StringRef("_local_bias")}) {
        const Symbol *Sym = findSymbol(Img, SymbolName);
        ASSERT_NE(Sym, nullptr);
        EXPECT_NE(Img.WritableRelocDataAddrs.count(Sym->Addr), 0u);
        EXPECT_EQ(Img.CodeRefTargets.count(Sym->Addr), 0u);
        EXPECT_EQ(Img.RelocDataAddrs.count(Sym->Addr), 0u);
      }
    }
  }
}

} // namespace
