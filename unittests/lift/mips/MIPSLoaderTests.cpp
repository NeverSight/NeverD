//===- MIPSLoaderTests.cpp - MIPS ELF profile and mapping tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently encoded ELF fixtures exercise width, byte order and ABI
/// admission without depending on a host MIPS cross compiler.
///
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramMetadata.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace neverd;

namespace {

using mips_test::ELFBytes;

class MIPSLoader : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSLoader, MapsLinkedImageAndReadsTargetOrderPointers) {
  ELFBytes File(GetParam());
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Arch, GetParam());
  EXPECT_EQ(Image->getPointerSize(), File.Is64 ? 8u : 4u);
  EXPECT_EQ(Image->Entry, 0x10100u);
  ASSERT_TRUE(Image->MIPS);
  EXPECT_EQ(Image->MIPS->ABI, File.Is64 ? MIPSABI::N64 : MIPSABI::O32);
  EXPECT_FALSE(Image->MIPS->HasABIFlags);
  const uint64_t Expected =
      File.Is64 ? (File.Little ? 0x8877665544332211ull : 0x1122334455667788ull)
                : (File.Little ? 0x44332211ull : 0x11223344ull);
  const auto Values = Image->readPtrArray(0x10110, 1);
  ASSERT_EQ(Values.size(), 1u);
  EXPECT_EQ(Values.front(), Expected);
  ASSERT_TRUE(Image->patchPtr(0x10110, 0x1020304050607080ull));
  const uint8_t *Patched = Image->readVA(0x10110, File.Is64 ? 8 : 4);
  ASSERT_NE(Patched, nullptr);
  EXPECT_EQ(Patched[0], File.Little ? 0x80 : (File.Is64 ? 0x10 : 0x50));
  EXPECT_EQ(Patched[(File.Is64 ? 8 : 4) - 1],
            File.Little ? (File.Is64 ? 0x10 : 0x50) : 0x80);
}

TEST_P(MIPSLoader, ProgramABIFlagsAgreeWithHeader) {
  ELFBytes File(GetParam());
  File.abiFlags();
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->MIPS);
  EXPECT_TRUE(Image->MIPS->HasABIFlags);
  EXPECT_EQ(Image->MIPS->FPABI, 3);
}

TEST_P(MIPSLoader, RuntimeSymbolModesAreValidatedWithoutSectionHeaders) {
  for (uint8_t Mode : {uint8_t(0x80), uint8_t(0xf0)}) {
    ELFBytes File(GetParam());
    File.dynamicSymbols(Mode);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Image));
    EXPECT_NE(llvm::toString(Image.takeError()).find("symbol encoding"),
              std::string::npos);
  }
  ELFBytes File(GetParam());
  File.dynamicSymbols(0);
  File.put(0x180 + (File.Is64 ? 8 : 4) * 5, 0, File.Is64 ? 8 : 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("bounded size"),
            std::string::npos);
}

TEST_P(MIPSLoader, DSPCapabilityRequiresClassic32BitProfileEvidence) {
  for (uint32_t ASE : {1u, 2u, 3u, 0x200u, 0x801u}) {
    ELFBytes File(GetParam());
    File.abiFlags();
    File.put(0xec, ASE, 4);
    if (ASE & 0x800)
      File.put(File.Is64 ? 48 : 36,
               (File.Is64 ? 0x80000000u : 0x70001000u) | 0x02000000, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    const bool Admitted = ASE == 1 && !File.Is64;
    ASSERT_EQ(bool(Image), Admitted);
    if (Admitted) {
      ASSERT_TRUE(Image->MIPS);
      EXPECT_TRUE(Image->MIPS->HasDSP);
    } else {
      llvm::consumeError(Image.takeError());
    }
    std::vector<LoadCandidate> Candidates;
    ELFLoader::identify(File.buffer(), Candidates);
    ASSERT_EQ(Candidates.size(), 1u);
    EXPECT_EQ(Candidates[0].Loadable, Admitted);
  }
}

TEST_P(MIPSLoader, MSARequiresRevisionFiveAndFP64VectorEvidence) {
  for (unsigned Variant = 0; Variant != 7; ++Variant) {
    ELFBytes File(GetParam());
    File.abiFlags();
    File.Bytes[0xe3] = Variant == 1 ? 2 : 5;
    File.Bytes[0xe5] = Variant == 2 ? 2 : 3;
    File.Bytes[0xe7] = Variant == 3 ? 3 : 6;
    File.put(0xec, Variant == 4 ? 0 : Variant == 5 ? 0x201 : 0x200, 4);
    if (Variant == 6)
      File.Bytes[0xe3] = 6;
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    const bool Admitted = Variant == 0 && !File.Is64;
    ASSERT_EQ(bool(Image), Admitted);
    if (Image) {
      EXPECT_TRUE(Image->MIPS->HasMSA);
      EXPECT_EQ(Image->MIPS->ISARevision, 5);
    } else {
      llvm::consumeError(Image.takeError());
    }
    std::vector<LoadCandidate> Candidates;
    ELFLoader::identify(File.buffer(), Candidates);
    ASSERT_EQ(Candidates.size(), 1u);
    EXPECT_EQ(Candidates[0].Loadable, Admitted);
  }
}

TEST_P(MIPSLoader, DynamicProgramTagsUseTheImageByteOrder) {
  ELFBytes File(GetParam());
  File.dynamicSymbols(0);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Dynamic = readELFProgramDynamicTable(*Image);
  ASSERT_TRUE(bool(Dynamic)) << llvm::toString(Dynamic.takeError());
  ASSERT_EQ(Dynamic->size(), 3u);
  EXPECT_EQ(Dynamic->front().Tag, 6);
  EXPECT_EQ(Dynamic->front().Value, 0x10200u);
  EXPECT_EQ(Dynamic->back().Tag, 0x70000011);
  EXPECT_EQ(Dynamic->back().Value, 2u);
}

TEST_P(MIPSLoader, GlobalGOTSuffixBindsImportsWithoutPerCellRelocations) {
  ELFBytes File(GetParam());
  File.dynamicGOT();
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const va_t Slot = 0x10380 + 2 * (File.Is64 ? 8 : 4);
  const auto *Imported = Image->findImportAt(Slot);
  ASSERT_NE(Imported, nullptr);
  EXPECT_EQ(Imported->Name, "fixture_import");
  ASSERT_TRUE(Image->ImportStorageSlots.count(Slot));
  EXPECT_EQ(Image->ImportStorageSlots.at(Slot).Name, "fixture_import");
  // A GOT with a first global index beyond the symbol table is malformed.
  File.put(0x180 + 15 * (File.Is64 ? 8 : 4), 3, File.Is64 ? 8 : 4);
  auto Invalid = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("inconsistent"),
            std::string::npos);
}

TEST_P(MIPSLoader, Packed64RelocationsAreRejectedExplicitly) {
  if (!isMIPS64(GetParam()))
    return;
  ELFBytes File(GetParam());
  File.dynamicGOT();
  // Runtime RELA metadata, without any section headers. r_info has a
  // target-order symbol word followed by ssym, type3, type2, type1 bytes.
  const std::pair<uint64_t, uint64_t> Extra[] = {
      {7, 0x103a0}, {8, 24}, {9, 24}, {0, 0}};
  for (unsigned I = 0; I < 4; ++I) {
    File.put(0x200 + 16 * I, Extra[I].first, 8);
    File.put(0x208 + 16 * I, Extra[I].second, 8);
  }
  File.put(120 + 32, 12 * 16, 8);
  File.put(120 + 40, 12 * 16, 8);
  File.put(0x3a0, 0x10380, 8);
  File.Bytes[0x3ae] = 18; // type2 = R_MIPS_64
  File.Bytes[0x3af] = 3;  // type1 = R_MIPS_REL32
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("packed MIPS64"),
            std::string::npos);
}

TEST_P(MIPSLoader, RuntimeRelocationsCannotSilentlyKeepUnresolvedAddends) {
  for (unsigned Type : {2u, 3u, 18u, 38u, 127u}) {
    for (bool HiddenBySections : {false, true}) {
      SCOPED_TRACE(Type);
      SCOPED_TRACE(HiddenBySections);
      ELFBytes File(GetParam());
      File.runtimeRelocation(Type, true);
      if (HiddenBySections) {
        // PT_DYNAMIC still names the relocation even when the only section
        // header left in the file is SHT_NULL.
        File.Bytes.resize(File.Is64 ? 0x440 : 0x428);
        File.put(File.Is64 ? 40 : 32, 0x400, File.Is64 ? 8 : 4);
        File.put(File.Is64 ? 60 : 48, 1, 2);
      }
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Image));
      EXPECT_NE(
          llvm::toString(Image.takeError()).find("MIPS runtime relocation"),
          std::string::npos);
    }
  }
}

TEST_P(MIPSLoader, GOTMayEndInTheZeroFilledLoadTailButNotOutsideTheMapping) {
  ELFBytes File(GetParam());
  File.dynamicGOT();
  const unsigned Width = File.Is64 ? 8 : 4;
  const size_t DataHeader = File.Is64 ? 176 : 116;
  const size_t FileSizeField = DataHeader + (File.Is64 ? 32 : 16);
  const size_t MemorySizeField = DataHeader + (File.Is64 ? 40 : 20);
  // The final GOT cell has a file-backed prefix and a zero-filled suffix,
  // as in the sectionless Archer C7 ubus executable.
  File.put(FileSizeField, 0x80 + 3 * Width - 3, Width);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_NE(Image->findImportAt(0x10380 + 2 * Width), nullptr);
  File.put(MemorySizeField, 0x80 + 3 * Width - 1, Width);
  auto Invalid = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("not mapped"),
            std::string::npos);
}

TEST_P(MIPSLoader, CompletePLTVeneerNamesOnlyItsBoundImportSlot) {
  ELFBytes File(GetParam());
  File.put(0x100, 0x3c0f0001, 4); // lui r15,1
  File.put(0x104, File.Is64 ? 0xddf90118 : 0x8df90118, 4);
  File.put(0x108, 0x03200008, 4); // jr t9
  File.put(0x10c, File.Is64 ? 0x65f80118 : 0x25f80118, 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Import Imported;
  Imported.Name = "fixture_import";
  Imported.IATAddr = 0x10118;
  Image->Imports.push_back(std::move(Imported));
  const auto *Found = Image->findImportAt(0x10100);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->Name, "fixture_import");
  EXPECT_EQ(Image->findImportAt(0x10104), nullptr);
  // A changed delay slot no longer states the PLT contract.
  Image->Segments[0].Data[0x10c] ^= 1;
  EXPECT_EQ(Image->findImportAt(0x10100), nullptr);
}

TEST_P(MIPSLoader, RejectsConflictingOrUnsupportedProfilesInBothEntrypoints) {
  const auto Reject = [&](ELFBytes &File, llvm::StringRef Expected) {
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Image));
    const std::string Error = llvm::toString(Image.takeError());
    EXPECT_NE(Error.find(Expected.str()), std::string::npos) << Error;
    std::vector<LoadCandidate> Rows;
    ELFLoader::identify(File.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    EXPECT_FALSE(Rows[0].Loadable);
    EXPECT_NE(Rows[0].Reason.find(Expected.str()), std::string::npos)
        << Rows[0].Reason;
  };
  for (const auto &[Bits, Reason] :
       std::array<std::pair<uint32_t, const char *>, 4>{
           {{0x20, "n32"},
            {0x02000000, "microMIPS"},
            {0x04000000, "MIPS16"},
            {0x00000100, "ABI"}}}) {
    ELFBytes File(GetParam());
    if ((Bits == 0x02000000 || Bits == 0x04000000) && !File.Is64)
      continue; // Compressed MIPS32 has separate admission/occurrence tests.
    File.put(File.Is64 ? 48 : 36, (File.Is64 ? 0x80000000 : 0x70001000) | Bits,
             4);
    Reject(File, Reason);
  }
  ELFBytes Revision(GetParam());
  Revision.put(Revision.Is64 ? 48 : 36, Revision.Is64 ? 0xa0000000 : 0x90001000,
               4);
  Reject(Revision, "matching");
  ELFBytes ClassConflict(GetParam());
  ClassConflict.put(ClassConflict.Is64 ? 48 : 36,
                    ClassConflict.Is64 ? 0x70000000 : 0x80001000, 4);
  Reject(ClassConflict, "matching");
  ELFBytes Metadata(GetParam());
  Metadata.abiFlags();
  Metadata.Bytes[0xe4] = Metadata.Is64 ? 1 : 2;
  Reject(Metadata, "conflict");
  ELFBytes Truncated(GetParam());
  Truncated.abiFlags();
  Truncated.Bytes.resize(0xf0);
  Reject(Truncated, "malformed .MIPS.abiflags");
  ELFBytes Relocatable(GetParam());
  Relocatable.put(16, 1, 2);
  Reject(Relocatable, "linked");
}

TEST(MicroMIPSLoader, MixedRuntimeSymbolsRetainExactModeAndBoundedRanges) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes File(Target);
    File.dynamicSymbols(0x80);
    File.put(36, 0x72001000, 4);
    File.put(0x214, 0x10108, 4); // Even st_value; st_other owns the mode.
    File.put(0x218, 6, 4);
    File.put(0x21c, 0x12, 1); // STT_FUNC, global
    File.put(0x21e, 1, 2);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Mode, InstructionMode::MixedMIPS);
    EXPECT_EQ(Image->instructionModeAt(0x10100), InstructionMode::MIPSClassic);
    EXPECT_EQ(Image->instructionModeAt(0x10108), InstructionMode::MicroMIPS);
    EXPECT_EQ(Image->instructionModeAt(0x1010c), InstructionMode::MicroMIPS);
    EXPECT_FALSE(Image->instructionModeAt(0x1010e));
    EXPECT_EQ(Image->instructionModeAt(0x1010e, InstructionMode::MicroMIPS),
              InstructionMode::MicroMIPS);
    EXPECT_FALSE(
        Image->instructionModeAt(0x10108, InstructionMode::MIPSClassic));
    EXPECT_FALSE(Image->instructionModeAt(0x10100, InstructionMode::MicroMIPS));
    EXPECT_TRUE(Image->isFunctionEntryCandidateAt(0x1010a));
    EXPECT_FALSE(Image->isFunctionEntryCandidateAt(0x1010e));
    File.put(24, 0x10109, 4);
    auto Tagged = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Tagged)) << llvm::toString(Tagged.takeError());
    EXPECT_EQ(Tagged->Entry, 0x10108u);
    File.put(24, 0x10108, 4); // Header says classic, symbol says micro.
    auto Conflict = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Conflict));
    EXPECT_NE(llvm::toString(Conflict.takeError()).find("conflicting"),
              std::string::npos);
  }
}

TEST(MIPS16Loader, HeaderSymbolsAndABIFlagsRetainTheMIPS16Family) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes File(Target);
    File.dynamicSymbols(
        0xf0); // Includes the microMIPS bit, but denotes MIPS16.
    File.put(36, 0x74001000, 4);
    File.put(0x214, 0x10108, 4);
    File.put(0x218, 6, 4);
    File.put(0x21c, 0x12, 1);
    File.put(0x21e, 1, 2);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->MIPS);
    EXPECT_TRUE(Image->MIPS->HasMIPS16);
    EXPECT_FALSE(Image->MIPS->HasMicroMIPS);
    EXPECT_EQ(Image->instructionModeAt(0x10100), InstructionMode::MIPSClassic);
    EXPECT_EQ(Image->instructionModeAt(0x10108), InstructionMode::MIPS16);
    EXPECT_EQ(Image->instructionModeAt(0x1010c), InstructionMode::MIPS16);
    EXPECT_FALSE(Image->instructionModeAt(0x1010e));
    EXPECT_FALSE(Image->instructionModeAt(0x10108, InstructionMode::MicroMIPS));
    File.put(24, 0x10109, 4);
    auto Tagged = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Tagged)) << llvm::toString(Tagged.takeError());
    EXPECT_EQ(Tagged->Entry, 0x10108u);
    for (uint32_t Flags : {0x70001000u, 0x72001000u, 0x76001000u}) {
      File.put(36, Flags, 4);
      auto Bad = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
    ELFBytes ABI(Target);
    ABI.abiFlags();
    ABI.put(36, 0x74001000, 4);
    ABI.put(24, 0x10101, 4);
    ABI.put(0xec, 0x400, 4); // AFL_ASE_MIPS16
    auto Matched = Loader.loadBuffer(ABI.buffer());
    ASSERT_TRUE(bool(Matched)) << llvm::toString(Matched.takeError());
    for (uint32_t ASE : {0u, 0x800u, 0xc00u, 0x401u, 0x600u}) {
      ABI.put(0xec, ASE, 4);
      auto Bad = Loader.loadBuffer(ABI.buffer());
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
  }
}

TEST(MIPS16Loader, E2RequiresMatchingExplicitABIRecords) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes F(Target);
    F.abiFlags();
    F.put(36, 0x74001000, 4);
    F.put(24, 0x10101, 4);
    ELFLoader L;
    for (uint32_t ASE :
         {0x400u, 0x4400u, 0x4440u, 0x440u, 0x4000u, 0x4800u, 0x4401u}) {
      F.put(0xec, ASE, 4);
      auto I = L.loadBuffer(F.buffer());
      const bool Valid = ASE == 0x400 || ASE == 0x4400 || ASE == 0x4440;
      ASSERT_EQ(bool(I), Valid);
      if (I)
        EXPECT_EQ(I->MIPS->HasMIPS16E2, bool(ASE & 0x4000));
      else
        llvm::consumeError(I.takeError());
    }
    F.put(0xec, 0x4400, 4);
    // Add a second program record with conflicting e2 evidence.
    F.put(44, 3, 2);
    F.put(116, 0x70000003, 4);
    F.put(120, 0xc0, 4);
    F.put(132, 24, 4);
    std::copy_n(F.Bytes.begin() + 0xe0, 24, F.Bytes.begin() + 0xc0);
    F.put(0xcc, 0x400, 4);
    auto Bad = L.loadBuffer(F.buffer());
    ASSERT_FALSE(bool(Bad));
    EXPECT_NE(llvm::toString(Bad.takeError()).find("conflicting"),
              std::string::npos);
  }
}

TEST(MIPS16Loader, DefinedGOTFunctionsSerializeTagsWithoutTaggingData) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Function : {false, true}) {
      ELFBytes File(Target);
      File.dynamicGOT();
      File.put(36, 0x74001000, 4);
      File.put(0x314, 0x10108, 4);
      File.put(0x318, 4, 4);
      File.put(0x31c, Function ? 0x12 : 0x11, 1);
      File.put(0x31d, Function ? 0xf0 : 0, 1);
      File.put(0x31e, 1, 2);
      File.put(0x388, Function ? 0x10109 : 0x10108, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const auto Pointers = Image->readPtrArray(0x10388, 1);
      ASSERT_EQ(Pointers.size(), 1u);
      EXPECT_EQ(Pointers.front(), Function ? 0x10109u : 0x10108u);
      File.put(0x388, Function ? 0x10108 : 0x10109, 4);
      auto Bad = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
}

TEST(MicroMIPSLoader, DefinedGOTFunctionsUseCallableTagsButDataDoesNot) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    for (bool Function : {false, true}) {
      SCOPED_TRACE(Function);
      ELFBytes File(Target);
      File.dynamicGOT();
      File.put(36, 0x72001000, 4);
      File.put(0x314, 0x10108, 4); // Even symbol value.
      File.put(0x318, 4, 4);
      File.put(0x31c, Function ? 0x12 : 0x11, 1); // STT_FUNC / STT_OBJECT
      File.put(0x31d, Function ? 0x80 : 0, 1);
      File.put(0x31e, 1, 2);
      File.put(0x388, Function ? 0x10109 : 0x10108, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const auto Pointers = Image->readPtrArray(0x10388, 1);
      ASSERT_EQ(Pointers.size(), 1u);
      EXPECT_EQ(Pointers.front(), Function ? 0x10109u : 0x10108u);
      File.put(0x388, Function ? 0x10108 : 0x10109, 4);
      auto Invalid = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Invalid));
      EXPECT_NE(llvm::toString(Invalid.takeError()).find("symbol resolution"),
                std::string::npos);
    }
  }
}

TEST(MicroMIPSLoader, HeaderAndABIFlagsMustAgreeAboutEncodingCapability) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes File(Target);
    File.abiFlags();
    File.put(36, 0x72001000, 4);
    File.put(24, 0x10101, 4);
    ELFLoader Loader;
    auto Missing = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Missing));
    llvm::consumeError(Missing.takeError());
    File.put(0xec, 0x800, 4);
    auto Admitted = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Admitted)) << llvm::toString(Admitted.takeError());
    EXPECT_EQ(Admitted->Entry, 0x10100u);
    EXPECT_EQ(Admitted->instructionModeAt(Admitted->Entry),
              InstructionMode::MicroMIPS);
    File.put(36, 0x70001000, 4);
    auto Contradiction = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Contradiction));
    llvm::consumeError(Contradiction.takeError());
  }
}

TEST_P(MIPSLoader, FrameLengthsPointersAndCFIUseTargetByteOrder) {
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
  std::vector<uint8_t> Bytes;
  const auto Append = [&](uint64_t Value, unsigned Width) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes.push_back(uint8_t(
          Value >> (8 * (archLittleEndian(GetParam()) ? I : Width - I - 1))));
  };
  Append(16, 4); // CIE length
  Append(0, 4);  // CIE id
  Bytes.insert(Bytes.end(), {1, 'z', 'R', 0, 1, 0x7c, 31, 1, 0, 0x0c, 29,
                             32}); // CFA = sp + 32
  const unsigned PointerBytes = Image.getPointerSize();
  const uint64_t FDEStart = Bytes.size();
  Append(10 + PointerBytes * 2, 4);
  Append(FDEStart + 4, 4); // backwards CIE offset
  Append(0x10100, PointerBytes);
  Append(0x180, PointerBytes);
  Append(0, 1); // empty augmentation
  Append(3, 1); // DW_CFA_advance_loc2
  Append(0x104, 2);
  Bytes.insert(Bytes.end(), {0x9f, 1}); // ra saved at CFA - 4
  Append(0, 4);
  const auto Frames =
      dwarf_eh::parseEHFrame(Image, {Bytes.data(), Bytes.size(), 0x30000}, {});
  EXPECT_EQ(Frames.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(Frames.CIEs.size(), 1u);
  EXPECT_EQ(Frames.CIEs[0].ReturnAddressRegister, 31u);
  ASSERT_EQ(Frames.FDEs.size(), 1u);
  EXPECT_EQ(Frames.FDEs[0].InitialLocation, 0x10100u);
  EXPECT_EQ(Frames.FDEs[0].AddressRange, 0x180u);
  ASSERT_EQ(Frames.FDEs[0].Instructions.size(), 2u);
  EXPECT_EQ(Frames.FDEs[0].Instructions.back().CodeOffset, 0x104u);
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSLoader,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
