//===- PPC32PLTTests.cpp - PPC32 System V import and caller proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check the normalized runtime relocation boundary and the reaching GOT at
/// two calls to one PIC stub. Linked-file execution lives in elf-integer.
///
//===----------------------------------------------------------------------===//

#include "../../lib/loader/ELF/ELFIntegerProfile.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
#include "PPC32PLTFixture.inc"

RelocationEntry jumpSlot(va_t VA, unsigned Index, llvm::StringRef Name) {
  RelocationEntry R;
  R.Type = llvm::ELF::R_PPC_JMP_SLOT;
  R.Address = VA;
  R.HasExplicitAddend = true;
  R.ELF.emplace();
  R.ELF->TableVirtualAddress = Rela;
  R.ELF->Source = ELFRelocationSource::ProgramDynamicTable;
  R.ELF->Ordinal = Index;
  auto &Symbol = R.ELF->Symbol.emplace();
  Symbol.Name = Name.str();
  Symbol.Type = llvm::ELF::STT_FUNC;
  Symbol.Binding = llvm::ELF::STB_GLOBAL;
  return R;
}

BinaryImage image(bool Little, bool Secure) {
  BinaryImage I;
  I.Format = BinaryFormat::ELF;
  I.Arch = Little ? Arch::PPC32LE : Arch::PPC32;
  I.Bits = Bitness::Bits32;
  I.LoadsAtLinkAddress = true;
  I.Entry = Text;
  auto &Code = I.addSegment(".text", Text, sizeof(Words),
                            SegmentFlags::Readable | SegmentFlags::Executable);
  Code.FileSz = sizeof(Words);
  Code.Data.resize(sizeof(Words));
  const auto Order = Little ? llvm::endianness::little : llvm::endianness::big;
  for (size_t N = 0; N < std::size(Words); ++N)
    llvm::support::endian::write32(Code.Data.data() + N * sizeof(uint32_t),
                                   Words[N], Order);
  auto Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  if (!Secure)
    Flags = Flags | SegmentFlags::Executable;
  I.addSegment(".plt", PLT,
               ClassicHeader + (LastShort + 1) * ShortStride + 2 * LongStride,
               Flags);
  return I;
}

MedFunc convert(const BinaryImage &Image) {
  Decoder D;
  EXPECT_TRUE(D.init(Image));
  auto Low = CFGBuilder().build(Image, D, Text, "caller");
  LowToMedConverter C;
  C.setBinaryImage(&Image);
  return C.convert(Low, Image.Arch, BinaryFormat::ELF);
}

TEST(PPC32PLT, SamePICVeneerUsesEachCallersReachingGOT) {
  for (bool Little : {false, true}) {
    auto I = image(Little, true);
    I.Relocations = {jumpSlot(PLT, 0, "first"), jumpSlot(PLT + 4, 1, "next")};
    ASSERT_EQ(
        llvm::toString(elf_loader::readPPC32Linkage(I, {PLT, Rela, 2, true})),
        "");
    ASSERT_NE(I.findImportStubAt(AbsoluteStub), nullptr);
    EXPECT_EQ(I.findImportStubAt(AbsoluteStub)->Name, "first");
    EXPECT_EQ(I.findImportStubAt(PICStub), nullptr);
    EXPECT_TRUE(I.isImportStubAt(PICStub));
    auto F = convert(I);
    std::vector<va_t> Slots;
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::INDIR_CALL && Op.Inputs[0].isConst())
          Slots.push_back(Op.Inputs[0].ConstVal);
    EXPECT_EQ(Slots, (std::vector<va_t>{PLT, PLT + 4}));

    auto Shared = image(Little, true);
    Shared.LoadsAtLinkAddress = false;
    Shared.Relocations = I.Relocations;
    ASSERT_EQ(llvm::toString(
                  elf_loader::readPPC32Linkage(Shared, {PLT, Rela, 2, true})),
              "");
    EXPECT_EQ(Shared.findImportStubAt(AbsoluteStub), nullptr);
    EXPECT_NO_THROW(convert(Shared));
    llvm::support::endian::write32(
        I.Segments.front().Data.data() + FinalGOTWord * sizeof(uint32_t),
        UseArgument, Little ? llvm::endianness::little : llvm::endianness::big);
    EXPECT_THROW(convert(I), LowToMedConversionError);
  }
}

TEST(PPC32PLT, BSSRelocationsOwnCodeAcrossTheStrideChange) {
  for (bool Little : {false, true}) {
    auto I = image(Little, false);
    const auto End = PLT + ClassicHeader + (LastShort + 1) * ShortStride;
    I.Relocations = {jumpSlot(End - ShortStride, LastShort, "last_short"),
                     jumpSlot(End, LastShort + 1, "first_long"),
                     jumpSlot(End + LongStride, LastShort + 2, "next_long")};
    const elf_loader::PPC32PLTTable T{PLT, Rela, LastShort + 3, false};
    ASSERT_EQ(llvm::toString(elf_loader::readPPC32Linkage(I, T)), "");
    EXPECT_TRUE(I.PPC32PLTSlots.empty());
    ASSERT_NE(I.findImportStubAt(End + LongStride), nullptr);
    EXPECT_EQ(I.findImportStubAt(End + LongStride)->IATAddr, 0u);
    EXPECT_EQ(I.findImportStubAt(End + LongStride)->Name, "next_long");

    auto Bad = image(Little, false);
    Bad.Relocations = {jumpSlot(End + ShortStride, LastShort + 2, "wrong")};
    EXPECT_NE(llvm::toString(elf_loader::readPPC32Linkage(Bad, T))
                  .find("invalid PPC32 PLT entry extent"),
              std::string::npos);
  }
}

TEST(PPC32PLT, OnlyTheRuntimeTableAndExactTransferEstablishAnImport) {
  for (bool Little : {false, true}) {
    auto I = image(Little, true);
    auto Static = jumpSlot(PLT, 0, "decoy");
    Static.ELF->Source = ELFRelocationSource::SectionTable;
    I.Relocations = {std::move(Static), jumpSlot(PLT, 0, "runtime")};
    llvm::support::endian::write32(
        I.Segments.front().Data.data() + LastInstruction * sizeof(uint32_t),
        BranchLinkCTR,
        Little ? llvm::endianness::little : llvm::endianness::big);
    const elf_loader::PPC32PLTTable T{PLT, Rela, 1, true};
    ASSERT_EQ(llvm::toString(elf_loader::readPPC32Linkage(I, T)), "");
    ASSERT_EQ(I.Imports.size(), 1u);
    EXPECT_EQ(I.Imports.front().Name, "runtime");
    EXPECT_EQ(I.findImportStubAt(AbsoluteStub), nullptr);
    auto Conflict = image(Little, true);
    Conflict.Relocations = {jumpSlot(PLT, 0, "one"), jumpSlot(PLT, 0, "two")};
    EXPECT_NE(llvm::toString(elf_loader::readPPC32Linkage(Conflict, T))
                  .find("duplicate PPC32 PLT entry"),
              std::string::npos);

    auto Overlap = image(Little, true);
    auto Copy = jumpSlot(PLT - ShortStride, 0, "before");
    Copy.Type = llvm::ELF::R_PPC_COPY;
    Copy.ELF->Symbol->Size = ShortStride + 1;
    Overlap.Relocations = {jumpSlot(PLT, 0, "import"), Copy};
    EXPECT_NE(llvm::toString(elf_loader::readPPC32Linkage(Overlap, T))
                  .find("conflicting writer"),
              std::string::npos);
    auto Adjacent = image(Little, true);
    Copy.ELF->Symbol->Size = ShortStride;
    Adjacent.Relocations = {jumpSlot(PLT, 0, "import"), Copy};
    EXPECT_EQ(llvm::toString(elf_loader::readPPC32Linkage(Adjacent, T)), "");
  }
}
} // namespace
