//===- RISCVPLTTests.cpp - RISC-V runtime jump slots and linker code ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/loader/ELF/ELFIntegerProfile.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/object/SectionNames.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
#include "RISCVPLTFixture.inc"

// The RV32 address calculation wraps past zero. The RV64 case must retain
// the high word of the PC. Each image has two imports and no sections.
BinaryImage image(bool Wide, bool Lazy) {
  BinaryImage I;
  I.Format = BinaryFormat::ELF;
  I.Arch = Wide ? Arch::RISCV64 : Arch::RISCV32;
  I.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
  I.Entry = Wide ? (uint64_t(1) << 32) + PLT0 : 0x1000;
  va_t Got = I.Entry - (PLT0 - GOT);
  if (!Wide)
    Got = uint32_t(Got);
  I.DynInfo.PltGotAddr = Got;
  auto &Code = I.addSegment(".plt", I.Entry, sizeof(Words32),
                            SegmentFlags::Readable | SegmentFlags::Executable);
  Code.FileSz = sizeof(Words32);
  Code.Data.resize(Code.FileSz);
  for (size_t N = 0; N < std::size(Words32); ++N)
    llvm::support::endian::write32le(Code.Data.data() + 4 * N,
                                     Wide ? Words64[N] : Words32[N]);
  const unsigned Ptr = I.getPointerSize();
  auto &Data = I.addSegment(".got.plt", Got, 4 * Ptr,
                            SegmentFlags::Readable | SegmentFlags::Writable);
  Data.FileSz = Data.Size;
  Data.Data.resize(Data.Size);
  for (unsigned N = 0; N < 2; ++N) {
    const auto Offset = (2 + N) * Ptr;
    if (Lazy) {
      if (Wide)
        llvm::support::endian::write64le(Data.Data.data() + Offset, I.Entry);
      else
        llvm::support::endian::write32le(Data.Data.data() + Offset, I.Entry);
    }
    auto &R = I.Relocations.emplace_back();
    R.Type = llvm::ELF::R_RISCV_JUMP_SLOT;
    R.Address = Got + Offset;
    R.HasExplicitAddend = true;
    R.SectionName = section_names::elf::RelaPlt;
    R.SymbolName = N ? "second" : "first";
    R.ELF.emplace();
    R.ELF->Source = ELFRelocationSource::ProgramDynamicTable;
    R.ELF->Ordinal = N;
    auto &Sym = R.ELF->Symbol.emplace();
    Sym.Type = llvm::ELF::STT_FUNC;
    Sym.Binding = llvm::ELF::STB_GLOBAL;
    Sym.Name = R.SymbolName;
  }
  return I;
}

TEST(RISCVPLT, BothEntryPathsReachTheRuntimeSlot) {
  for (bool Wide : {false, true})
    for (bool Lazy : {false, true}) {
      auto I = image(Wide, Lazy);
      std::reverse(I.Relocations.begin(), I.Relocations.end());
      ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(I)), "");
      EXPECT_EQ(I.findImportStubAt(I.Entry), nullptr);
      for (unsigned N = 0; N < 2; ++N) {
        const va_t Stub = I.Entry + 32 + 16 * N;
        const Import *Found = I.findImportStubAt(Stub);
        ASSERT_NE(Found, nullptr);
        EXPECT_EQ(Found->Name, N ? "second" : "first");
        EXPECT_EQ(Found->IATAddr,
                  I.DynInfo.PltGotAddr + (N + 2) * I.getPointerSize());
        EXPECT_EQ(I.findImportStubAt(Stub + 4), nullptr);
      }
    }
}

TEST(RISCVPLT, SectionlessScanRespectsAlignmentAndTheLastFileByte) {
  for (bool Wide : {false, true})
    for (bool Compressed : {false, true})
      for (bool Truncate : {false, true}) {
        auto I = image(Wide, false);
        I.RISCV.Compressed = Compressed;
        // Move PLT0 down half a word, keeping the GOT and both slots fixed.
        I.Entry -= 2;
        auto &Code = I.Segments.front();
        for (unsigned N : {2u, 4u, 9u, 13u}) {
          auto *Insn = Code.Data.data() + N * sizeof(uint32_t);
          llvm::support::endian::write32le(
              Insn, llvm::support::endian::read32le(Insn) + (2u << 20));
        }
        constexpr size_t HeaderBytes = 8 * sizeof(uint32_t);
        std::vector<uint8_t> Entries(Code.Data.begin() + HeaderBytes,
                                     Code.Data.end());
        Code.Data.resize(HeaderBytes);
        // An earlier exact SUB belongs to unrelated instructions. The
        // actual resolver starts at the last possible position in this map.
        constexpr size_t Prefix = 17;
        Code.Data.insert(Code.Data.begin(), Prefix, 0);
        llvm::support::endian::write32le(Code.Data.data() + 4, Words32[1]);
        Code.VA = I.Entry - Prefix;
        Code.Size = Code.Data.size();
        Code.FileSz = Code.Size - Truncate;
        auto &Stubs = I.addSegment("stubs", I.Entry + HeaderBytes,
                                   Entries.size(), Code.Flags);
        Stubs.FileSz = Stubs.Size;
        Stubs.Data = std::move(Entries);
        ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(I)), "");
        EXPECT_EQ(I.ImportStubIndices.size(),
                  Compressed && !Truncate ? 2u : 0u);
      }
}

TEST(RISCVPLT, UnrelaxedTailCallRetainsTheCallerAndTheImportedFunction) {
  for (bool Wide : {false, true}) {
    auto I = image(Wide, true);
    ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(I)), "");
    I.Entry -= 32;
    auto &Text =
        I.addSegment(".text", I.Entry, sizeof(TailCaller),
                     SegmentFlags::Readable | SegmentFlags::Executable);
    Text.FileSz = Text.Size;
    Text.Data.resize(Text.FileSz);
    for (size_t N = 0; N < std::size(TailCaller); ++N)
      llvm::support::endian::write32le(Text.Data.data() + N * sizeof(uint32_t),
                                       TailCaller[N]);
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {I.Entry};
    Options.LiftMode = true;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    auto Result = Pipeline().run(I, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.LlvmModule);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    auto *Caller = Result.LlvmModule->getFunction(Result.MedFuncs.front().Name);
    ASSERT_TRUE(Caller && !Caller->isDeclaration());
    auto *First = Result.LlvmModule->getFunction("first");
    ASSERT_TRUE(First && First->isDeclaration());
    EXPECT_TRUE(llvm::any_of(First->users(), [&](const auto *User) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
      return Call && Call->getCalledFunction() == First &&
             Call->getFunction() == Caller;
    }));
  }
}

TEST(RISCVPLT, RejectsMismatchedInstructionsAndRelocationEvidence) {
  for (bool Wide : {false, true}) {
    for (unsigned Word : {0u, 2u, 7u, 8u, 9u, 10u, 11u}) {
      auto I = image(Wide, true);
      I.Segments.front().Data[4 * Word] ^= 1;
      ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(I)), "");
      EXPECT_EQ(I.findImportStubAt(I.Entry + 32), nullptr);
    }
    auto Width = image(Wide, false);
    llvm::support::endian::write32le(Width.Segments.front().Data.data() + 36,
                                     Wide ? Words32[9] : Words64[9]);
    ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(Width)), "");
    EXPECT_EQ(Width.findImportStubAt(Width.Entry + 32), nullptr);
    auto Order = image(Wide, true);
    std::swap(Order.Relocations[0].ELF->Ordinal,
              Order.Relocations[1].ELF->Ordinal);
    ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(Order)), "");
    EXPECT_TRUE(Order.ImportStubIndices.empty());
    auto Section = image(Wide, true);
    Section.Relocations[0].ELF->Source = ELFRelocationSource::SectionTable;
    Section.Relocations[1].ELF->Symbol->Other = llvm::ELF::STO_RISCV_VARIANT_CC;
    ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(Section)), "");
    EXPECT_TRUE(Section.ImportStubIndices.empty());
  }
}

TEST(RISCVPLT, RejectsOverlappingRuntimeWrites) {
  auto I = image(true, false);
  auto Copy = I.Relocations.front();
  Copy.Type = llvm::ELF::R_RISCV_COPY;
  Copy.Address -= I.getPointerSize();
  Copy.ELF->Symbol->Size = I.getPointerSize() + 1;
  I.Relocations.push_back(Copy);
  EXPECT_NE(llvm::toString(elf_loader::readRISCVLinkage(I))
                .find("conflicting writer"),
            std::string::npos);
  auto Code = image(true, true);
  Copy.Address = Code.Entry + 32;
  Code.Relocations.push_back(Copy);
  ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(Code)), "");
  EXPECT_EQ(Code.findImportStubAt(Code.Entry + 32), nullptr);
  EXPECT_NE(Code.findImportStubAt(Code.Entry + 48), nullptr);
}

TEST(RISCVPLT, PackedRelativeWritesAlsoOwnTheirFullWidth) {
  for (bool Wide : {false, true}) {
    auto I = image(Wide, true);
    const auto Slot = I.Relocations[1].Address;
    EXPECT_NE(llvm::toString(elf_loader::readRISCVLinkage(I, {Slot}))
                  .find("conflicting writer"),
              std::string::npos);
    auto Code = image(Wide, true);
    ASSERT_EQ(llvm::toString(elf_loader::readRISCVLinkage(
                  Code, {Code.Entry + 48 - Code.getPointerSize()})),
              "");
    EXPECT_EQ(Code.findImportStubAt(Code.Entry + 32), nullptr);
    EXPECT_NE(Code.findImportStubAt(Code.Entry + 48), nullptr);
  }
}
} // namespace
