//===- SourcePatchLayoutTests.cpp - Native image layout growth -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise file-pointer preservation and transactional layout rejection.
//===----------------------------------------------------------------------===//

#include "../lib/backend/codegen/MachO/MachODependencyState.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/STLExtras.h"

#include <cstring>

namespace {
using namespace neverd;
using namespace llvm::object;

std::vector<uint8_t> tightPE(bool Is64) {
  std::vector<uint8_t> B(0x900, 0);
  auto *DOS = reinterpret_cast<dos_header *>(B.data());
  B[0] = 'M';
  B[1] = 'Z';
  DOS->AddressOfNewExeHeader = 0x80;
  std::memcpy(B.data() + 0x80, llvm::COFF::PEMagic, 4);
  auto *File = reinterpret_cast<coff_file_header *>(B.data() + 0x84);
  File->NumberOfSections = 3;
  File->Machine = Is64 ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                       : llvm::COFF::IMAGE_FILE_MACHINE_I386;
  const auto Init = [&](auto *Opt) {
    Opt->Magic =
        Is64 ? llvm::COFF::PE32Header::PE32_PLUS : llvm::COFF::PE32Header::PE32;
    Opt->FileAlignment = 0x200;
    Opt->SectionAlignment = 0x1000;
    Opt->SizeOfHeaders = 0x200;
    Opt->SizeOfImage = 0x4000;
    Opt->NumberOfRvaAndSize = 16;
    File->SizeOfOptionalHeader = sizeof(*Opt) + 16 * sizeof(data_directory);
  };
  if (Is64)
    Init(reinterpret_cast<pe32plus_header *>(B.data() + 0x98));
  else
    Init(reinterpret_cast<pe32_header *>(B.data() + 0x98));
  auto PE = locatePEHeaders(B.data(), B.size());
  auto *Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
  for (unsigned I = 0; I < 3; ++I) {
    auto &S = Sections[I];
    S.VirtualAddress = 0x1000 * (I + 1);
    S.VirtualSize = 0x200;
    S.SizeOfRawData = 0x200;
    S.PointerToRawData = 0x200 * (I + 1);
    std::fill_n(B.begin() + S.PointerToRawData, 0x200, 0x41 + I);
  }
  std::fill(B.begin() + 0x800, B.end(), 0xdd);
  return B;
}

TEST(SourcePatchLayout, TightPE32AndPE64GrowWithoutChangingSectionRVAs) {
  for (bool Is64 : {false, true}) {
    auto B = tightPE(Is64);
    const auto Before = B;
    auto G = ensureCOFFSectionHeaderCapacity(B, 3);
    ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
    EXPECT_EQ(G->addedFileBytes(), 0x200u);
    EXPECT_EQ(G->NewPEOffset, 0x200u);
    EXPECT_EQ(B.size(), Before.size() + 0x200);
    auto PE = locatePEHeaders(B.data(), B.size());
    EXPECT_EQ(getPESizeOfHeaders(PE), 0x400u);
    const auto *Sections =
        reinterpret_cast<const coff_section *>(PE.SectionTable);
    for (unsigned I = 0; I < 3; ++I) {
      EXPECT_EQ(Sections[I].VirtualAddress, 0x1000u * (I + 1));
      EXPECT_EQ(Sections[I].PointerToRawData, 0x200u * (I + 2));
    }
    EXPECT_TRUE(
        std::equal(Before.begin() + 0x200, Before.end(), B.begin() + 0x400));
    const auto Grown = B;
    auto Again = ensureCOFFSectionHeaderCapacity(B, 0);
    ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
    EXPECT_EQ(Again->addedFileBytes(), 0u);
    EXPECT_EQ(B, Grown);
  }
}

TEST(SourcePatchLayout, PreservesOccupiedHeaderBytesByMovingNTHeaders) {
  auto B = tightPE(false);
  // Nonzero bytes immediately after the old table must not be used as padding.
  const std::string Payload = "header payload!";
  std::copy(Payload.begin(), Payload.end(), B.begin() + 0x1f0);
  const auto Before = B;
  auto G = ensureCOFFSectionHeaderCapacity(B, 2);
  ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
  EXPECT_EQ(G->NewPEOffset, 0x200u);
  EXPECT_EQ(G->NewSizeOfHeaders, 0x400u);
  auto PE = locatePEHeaders(B.data(), B.size());
  EXPECT_EQ(PE.PeOffset, 0x200u);
  EXPECT_TRUE(std::equal(Payload.begin(), Payload.end(), B.begin() + 0x1f0));
  EXPECT_TRUE(
      std::equal(Before.begin() + 0x200, Before.end(), B.begin() + 0x400));
  EXPECT_EQ(getPESizeOfImage(PE), 0x4000u);
}

TEST(SourcePatchLayout, DoesNotTreatHeaderResidentZeroMetadataAsPadding) {
  auto B = tightPE(false);
  auto PE = locatePEHeaders(B.data(), B.size());
  setPEDataDirectory(PE, llvm::COFF::RESOURCE_TABLE, 0x1f0, 16);
  auto G = ensureCOFFSectionHeaderCapacity(B, 1);
  ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
  EXPECT_EQ(G->NewPEOffset, 0x200u);
  PE = locatePEHeaders(B.data(), B.size());
  EXPECT_EQ(getPEDataDirectory(PE, llvm::COFF::RESOURCE_TABLE)
                ->RelativeVirtualAddress,
            0x1f0u);
  EXPECT_TRUE(llvm::all_of(llvm::ArrayRef<uint8_t>(B).slice(0x1f0, 16),
                           [](uint8_t Byte) { return Byte == 0; }));
}

TEST(SourcePatchLayout, PreservesNestedResourcePayloadInHeader) {
  auto B = tightPE(false);
  auto PE = locatePEHeaders(B.data(), B.size());
  setPEDataDirectory(PE, llvm::COFF::RESOURCE_TABLE, 0x2000, 40);
  // A root resource directory in section two references a data entry, whose
  // payload is the zero-filled sixteen bytes following the old NT headers.
  std::fill_n(B.begin() + 0x400, 40, 0);
  writeLE<uint16_t>(B.data() + 0x40e, 1);
  writeLE<uint32_t>(B.data() + 0x410, 1);
  writeLE<uint32_t>(B.data() + 0x414, 24);
  writeLE<uint32_t>(B.data() + 0x418, 0x1f0);
  writeLE<uint32_t>(B.data() + 0x41c, 16);
  const auto Before = B;
  auto G = ensureCOFFSectionHeaderCapacity(B, 1);
  ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
  EXPECT_EQ(G->NewPEOffset, 0x200u);
  EXPECT_TRUE(std::equal(Before.begin() + 0x1f0, Before.begin() + 0x200,
                         B.begin() + 0x1f0));
  EXPECT_EQ(readLE<uint32_t>(B.data() + 0x618), 0x1f0u);
}

TEST(SourcePatchLayout, RefusesLowAlignmentEvenWhenVirtualHeadersHaveRoom) {
  auto B = tightPE(false);
  B.resize(0x3200);
  auto PE = locatePEHeaders(B.data(), B.size());
  reinterpret_cast<pe32_header *>(PE.OptionalHeader)->SectionAlignment = 0x200;
  auto *Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
  for (unsigned I = 0; I < 3; ++I)
    Sections[I].PointerToRawData = Sections[I].VirtualAddress;
  const auto Before = B;
  auto G = ensureCOFFSectionHeaderCapacity(B, 1);
  ASSERT_FALSE(bool(G));
  EXPECT_NE(llvm::toString(G.takeError()).find("low-alignment"),
            std::string::npos);
  EXPECT_EQ(B, Before);
}

TEST(SourcePatchLayout, MovesDebugCOFFAndCertificateFilePointersWithOverlay) {
  auto B = tightPE(true);
  auto PE = locatePEHeaders(B.data(), B.size());
  auto *Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
  Sections[0].PointerToRelocations = 0x800;
  Sections[0].NumberOfRelocations = 1;
  Sections[0].PointerToLinenumbers = 0x810;
  Sections[0].NumberOfLinenumbers = 1;
  PE.FileHeader->PointerToSymbolTable = 0x820;
  PE.FileHeader->NumberOfSymbols = 1;
  std::fill_n(B.begin() + 0x820, 18, 0);
  writeLE<uint32_t>(B.data() + 0x832, 4);
  setPEDataDirectory(PE, llvm::COFF::CERTIFICATE_TABLE, 0x840, 0x20);
  setPEDataDirectory(PE, llvm::COFF::DEBUG_DIRECTORY, 0x2000,
                     sizeof(debug_directory));
  auto *Debug = reinterpret_cast<debug_directory *>(B.data() + 0x400);
  *Debug = {};
  Debug->SizeOfData = 8;
  Debug->AddressOfRawData = 0x2040;
  Debug->PointerToRawData = 0x440;
  const auto Before = B;
  auto G = ensureCOFFSectionHeaderCapacity(B, 1);
  ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
  PE = locatePEHeaders(B.data(), B.size());
  Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
  EXPECT_EQ(Sections[0].PointerToRelocations, 0xa00u);
  EXPECT_EQ(Sections[0].PointerToLinenumbers, 0xa10u);
  EXPECT_EQ(PE.FileHeader->PointerToSymbolTable, 0xa20u);
  EXPECT_EQ(getPEDataDirectory(PE, llvm::COFF::CERTIFICATE_TABLE)
                ->RelativeVirtualAddress,
            0xa40u);
  Debug = reinterpret_cast<debug_directory *>(B.data() + 0x600);
  EXPECT_EQ(Debug->PointerToRawData, 0x640u);
  EXPECT_EQ(Debug->AddressOfRawData, 0x2040u);
  EXPECT_TRUE(
      std::equal(Before.begin() + 0x800, Before.end(), B.begin() + 0xa00));
}

TEST(SourcePatchLayout, MovesAuxiliaryFunctionLinePointerButKeepsSymbolIndex) {
  auto B = tightPE(true);
  auto PE = locatePEHeaders(B.data(), B.size());
  auto *Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
  Sections[0].PointerToLinenumbers = 0x810;
  Sections[0].NumberOfLinenumbers = 1;
  PE.FileHeader->PointerToSymbolTable = 0x820;
  PE.FileHeader->NumberOfSymbols = 2;
  auto *Symbol = reinterpret_cast<coff_symbol16 *>(B.data() + 0x820);
  *Symbol = {};
  Symbol->Type = 0x20;
  Symbol->StorageClass = llvm::COFF::IMAGE_SYM_CLASS_EXTERNAL;
  Symbol->SectionNumber = 1;
  Symbol->NumberOfAuxSymbols = 1;
  auto *Aux =
      reinterpret_cast<coff_aux_function_definition *>(B.data() + 0x832);
  *Aux = {};
  Aux->PointerToLinenumber = 0x810;
  Aux->PointerToNextFunction = 1;
  writeLE<uint32_t>(B.data() + 0x844, 4);
  auto G = ensureCOFFSectionHeaderCapacity(B, 1);
  ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
  Aux = reinterpret_cast<coff_aux_function_definition *>(B.data() + 0xa32);
  EXPECT_EQ(Aux->PointerToLinenumber, 0xa10u);
  EXPECT_EQ(Aux->PointerToNextFunction, 1u);
}

TEST(SourcePatchLayout, RefusesUnrepresentableGrowthWithoutChangingBytes) {
  for (unsigned Case = 0; Case < 6; ++Case) {
    auto B = tightPE(true);
    auto PE = locatePEHeaders(B.data(), B.size());
    auto *Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
    switch (Case) {
    case 0: // Native loader section-count bound.
      break;
    case 1: // Malformed backing extent.
      Sections[0].PointerToRawData = 0x1000;
      break;
    case 2: // Overlapping file-backed sections.
      Sections[1].PointerToRawData = Sections[0].PointerToRawData;
      break;
    case 3: // Misaligned declared header extent.
      reinterpret_cast<pe32plus_header *>(PE.OptionalHeader)->SizeOfHeaders =
          0x210;
      break;
    case 4: // Low-alignment images have no room in their virtual header
            // mapping.
      reinterpret_cast<pe32plus_header *>(PE.OptionalHeader)->SectionAlignment =
          0x200;
      Sections[0].VirtualAddress = 0x200;
      break;
    case 5: // File pointer reaches beyond the file.
      Sections[0].PointerToRelocations = 0x8ff;
      Sections[0].NumberOfRelocations = 1;
      break;
    }
    const auto Before = B;
    auto G = ensureCOFFSectionHeaderCapacity(B, Case == 0 ? 94 : 1);
    EXPECT_FALSE(bool(G)) << Case;
    llvm::consumeError(G.takeError());
    EXPECT_EQ(B, Before) << Case;
  }
}

TEST(SourcePatchLayout, RefusesInconsistentDebugIdentityWithoutChangingBytes) {
  auto B = tightPE(true);
  auto PE = locatePEHeaders(B.data(), B.size());
  setPEDataDirectory(PE, llvm::COFF::DEBUG_DIRECTORY, 0x2000,
                     sizeof(debug_directory));
  auto *Debug = reinterpret_cast<debug_directory *>(B.data() + 0x400);
  *Debug = {};
  Debug->SizeOfData = 8;
  Debug->AddressOfRawData = 0x2040;
  Debug->PointerToRawData = 0x450;
  const auto Before = B;
  auto G = ensureCOFFSectionHeaderCapacity(B, 1);
  ASSERT_FALSE(bool(G));
  EXPECT_NE(llvm::toString(G.takeError()).find("disagree"), std::string::npos);
  EXPECT_EQ(B, Before);
}

std::vector<uint8_t> headerMachO(bool BlockTextTail = false) {
  using namespace llvm::MachO;
  std::vector<uint8_t> B(0x3000, 0);
  auto *H = reinterpret_cast<mach_header_64 *>(B.data());
  H->magic = MH_MAGIC_64;
  H->cputype = CPU_TYPE_X86_64;
  H->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
  H->filetype = MH_EXECUTE;
  H->flags = MH_PIE;
  H->ncmds = BlockTextTail ? 6 : 5;
  uint64_t Off = sizeof(*H);
  auto Segment = [&](const char *Name, uint64_t VA, uint64_t Size,
                     uint64_t FileOff, uint64_t FileSize, bool Text) {
    auto *G = reinterpret_cast<segment_command_64 *>(B.data() + Off);
    G->cmd = LC_SEGMENT_64;
    G->cmdsize =
        sizeof(*G) + (Text ? (BlockTextTail ? 2 : 1) * sizeof(section_64) : 0);
    std::strcpy(G->segname, Name);
    G->vmaddr = VA;
    G->vmsize = Size;
    G->fileoff = FileOff;
    G->filesize = FileSize;
    G->initprot = G->maxprot = Text                               ? 5
                               : std::strcmp(Name, "__DATA") == 0 ? 3
                               : FileSize                         ? 1
                                                                  : 0;
    G->nsects = Text ? (BlockTextTail ? 2 : 1) : 0;
    if (Text) {
      auto *S = reinterpret_cast<section_64 *>(G + 1);
      std::strcpy(S->segname, "__TEXT");
      std::strcpy(S->sectname, "__text");
      S->addr = VA + 0x800;
      S->size = 16;
      S->offset = 0x800;
      S->flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
      if (BlockTextTail) {
        ++S;
        std::strcpy(S->segname, "__TEXT");
        std::strcpy(S->sectname, "__unwind_info");
        S->addr = VA + 0x820;
        S->offset = 0x820;
        S->size = 68;
        S->align = 2;
      }
    }
    Off += G->cmdsize;
  };
  Segment("__PAGEZERO", 0, 0x100000000, 0, 0, false);
  Segment("__TEXT", 0x100000000, BlockTextTail ? 0x1000 : 0x2000, 0,
          BlockTextTail ? 0x1000 : 0x2000, true);
  if (BlockTextTail) {
    Segment("__DATA", 0x100001000, 0x1000, 0x1000, 0x1000, false);
    std::fill(B.begin() + 0x1000, B.begin() + 0x2000, 0xa5);
    // One regular compact-unwind entry and its sentinel, with no spare page.
    auto *U = B.data() + 0x820;
    writeLE<uint32_t>(U, 1);
    writeLE<uint32_t>(U + 4, 28);
    writeLE<uint32_t>(U + 12, 28);
    writeLE<uint32_t>(U + 20, 28);
    writeLE<uint32_t>(U + 24, 2);
    writeLE<uint32_t>(U + 28, 0x800);
    writeLE<uint32_t>(U + 32, 52);
    writeLE<uint32_t>(U + 36, 52);
    writeLE<uint32_t>(U + 40, 0x810);
    writeLE<uint32_t>(U + 48, 52);
    writeLE<uint32_t>(U + 52, 2);
    writeLE<uint16_t>(U + 56, 8);
    writeLE<uint16_t>(U + 58, 1);
    writeLE<uint32_t>(U + 60, 0x800);
    writeLE<uint32_t>(U + 64, 0x01000000);
  }
  Segment("__LINKEDIT", 0x100002000, 0x1000, 0x2000, 0x1000, false);
  auto *F = reinterpret_cast<linkedit_data_command *>(B.data() + Off);
  F->cmd = LC_FUNCTION_STARTS;
  F->cmdsize = sizeof(*F);
  F->dataoff = 0x2100;
  F->datasize = 3;
  Off += sizeof(*F);
  B[0x2100] = 0x80;
  B[0x2101] = 0x10;
  auto *M = reinterpret_cast<entry_point_command *>(B.data() + Off);
  M->cmd = LC_MAIN;
  M->cmdsize = sizeof(*M);
  M->entryoff = 0x800;
  Off += sizeof(*M);
  H->sizeofcmds = Off - sizeof(*H);
  std::fill(B.begin() + 0x800, B.begin() + 0x810, 0x90);
  B[0x80f] = 0xc3;
  return B;
}
TEST(SourcePatchLayout, MachOHeaderGrowthKeepsCodeAtOriginalVA) {
  auto B = headerMachO();
  auto H = parseMachOHeader(B.data(), B.size());
  std::fill(B.begin() + H.HeaderSize + H.SizeOfCmds, B.begin() + 0x800, 0x5a);
  const auto Old = B;
  auto R = reserveMachOHeaderStorage(B, Arch::X64, 512);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_TRUE(*R);
  uint64_t HeaderVA = 0;
  forEachMachOLoadCommand(
      B.data(), B.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const auto G = readMachOSegment(C, Is64);
        if (readMachOName(G.SegName) != "__TEXT")
          return;
        HeaderVA = G.VMAddr;
        EXPECT_EQ(G.VMAddr + 0x1800, 0x100000800ULL);
      });
  EXPECT_EQ(HeaderVA, 0xfffff000ULL);
  EXPECT_TRUE(
      std::equal(Old.begin() + 0x800, Old.begin() + 0x810, B.begin() + 0x1800));

  // The unwind owner must adopt header growth itself when a DATA mapping
  // blocks the TEXT tail, then retain the rebased original compact records.
  auto Tight = headerMachO(true);
  const auto Before = Tight;
  auto Grown = reserveMachOSourceUnwind(Tight, Arch::X64, 4096, 4);
  ASSERT_TRUE(bool(Grown)) << llvm::toString(Grown.takeError());
  ASSERT_TRUE(*Grown);
  uint64_t TextVA = 0, CodeOffset = 0;
  forEachMachOLoadCommand(
      Tight.data(), Tight.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const auto G = readMachOSegment(C, Is64);
        if (readMachOName(G.SegName) == "__DATA") {
          EXPECT_EQ(G.VMAddr, 0x100001000ULL);
          EXPECT_TRUE(std::equal(Before.begin() + 0x1000,
                                 Before.begin() + 0x2000,
                                 Tight.begin() + G.FileOff));
        }
        if (readMachOName(G.SegName) != "__TEXT")
          return;
        TextVA = G.VMAddr;
        const auto *S = reinterpret_cast<const llvm::MachO::section_64 *>(
            C + sizeof(llvm::MachO::segment_command_64));
        for (unsigned I = 0; I < G.NSects; ++I)
          if (readMachOName(S[I].sectname) == "__text") {
            EXPECT_EQ(S[I].addr, 0x100000800ULL);
            CodeOffset = S[I].offset;
          }
      });
  ASSERT_NE(CodeOffset, 0u);
  EXPECT_TRUE(std::equal(Before.begin() + 0x800, Before.begin() + 0x810,
                         Tight.begin() + CodeOffset));
  auto Region = findMachOCompactUnwindRegion(Tight);
  ASSERT_TRUE(bool(Region)) << llvm::toString(Region.takeError());
  ASSERT_TRUE(bool(*Region));
  auto Records =
      macho_unwind::parseCompactUnwindRaw(llvm::ArrayRef<uint8_t>(Tight).slice(
          (**Region).SectionFileOff, (**Region).SectionSize));
  ASSERT_TRUE(bool(Records)) << llvm::toString(Records.takeError());
  ASSERT_FALSE(Records->Index.empty());
  EXPECT_EQ(TextVA + Records->Index.front().FunctionOffset, 0x100000800ULL);

  // Variable-length LINKEDIT records must retain native pointer alignment in
  // either command order, even when the preceding rewritten payload is odd.
  for (bool ExportsFirst : {false, true}) {
    using namespace llvm::MachO;
    auto Image = headerMachO();
    auto *Header = reinterpret_cast<mach_header_64 *>(Image.data());
    linkedit_data_command *Starts = nullptr;
    forEachMachOLoadCommand(Image.data(), Image.size(),
                            [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
                              if (ID == LC_FUNCTION_STARTS)
                                Starts =
                                    reinterpret_cast<linkedit_data_command *>(
                                        Image.data() + (C - Image.data()));
                            });
    ASSERT_NE(Starts, nullptr);
    auto *Exports = reinterpret_cast<linkedit_data_command *>(
        Image.data() + sizeof(*Header) + Header->sizeofcmds);
    *Exports = {LC_DYLD_EXPORTS_TRIE, sizeof(*Exports), 0x2200, 11};
    const uint8_t Trie[] = {0, 1, '_', 'f', 0, 6, 3, 0, 0x80, 0x10, 0};
    std::copy(std::begin(Trie), std::end(Trie), Image.begin() + 0x2200);
    Header->sizeofcmds += sizeof(*Exports);
    ++Header->ncmds;
    if (ExportsFirst)
      std::swap(*Starts, *Exports);
    std::fill(Image.begin() + sizeof(*Header) + Header->sizeofcmds,
              Image.begin() + 0x800, 0x5a);
    auto G = reserveMachOHeaderStorage(Image, Arch::X64, 512);
    ASSERT_TRUE(bool(G)) << llvm::toString(G.takeError());
    ASSERT_TRUE(*G);
    forEachMachOLoadCommand(
        Image.data(), Image.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID == LC_FUNCTION_STARTS || ID == LC_DYLD_EXPORTS_TRIE) {
            const auto *L = reinterpret_cast<const linkedit_data_command *>(C);
            EXPECT_EQ(L->dataoff % 8, 0u) << ID;
            EXPECT_GT(L->datasize, 0u);
          }
        });
  }
}
TEST(SourcePatchLayout,
     MachORefusesHeaderRelativeArithmeticWithoutChangingInput) {
  auto B = headerMachO();
  auto H = parseMachOHeader(B.data(), B.size());
  std::fill(B.begin() + H.HeaderSize + H.SizeOfCmds, B.begin() + 0x800, 0x5a);
  // lea header(%rip),rax; add $0x800,rax; ret must keep its old result.
  const uint8_t Code[] = {0x48, 0x8d, 0x05, 0xf9, 0xf7, 0xff, 0xff, 0x48,
                          0x05, 0x00, 0x08, 0x00, 0x00, 0xc3, 0x90, 0x90};
  std::copy(std::begin(Code), std::end(Code), B.begin() + 0x800);
  const auto Old = B;
  auto R = reserveMachOHeaderStorage(B, Arch::X64, 512);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("relocated Mach header"),
            std::string::npos);
  EXPECT_EQ(B, Old);
  auto Blocked = headerMachO(true);
  std::copy(std::begin(Code), std::end(Code), Blocked.begin() + 0x800);
  const auto Before = Blocked;
  auto Unwind = reserveMachOSourceUnwind(Blocked, Arch::X64, 4096, 4);
  ASSERT_FALSE(bool(Unwind));
  llvm::consumeError(Unwind.takeError());
  EXPECT_EQ(Blocked, Before);
}

TEST(SourcePatchLayout, FailedLoadCommandReservationRollsBackUnwindGrowth) {
  const auto Original = headerMachO(true);
  const MachOUnwindFunctionCounts Added{1, 0};
  auto Bytes = Original;
  auto Space =
      reserveMachOSourceUnwind(Bytes, Arch::X64, sizeof(uint32_t), Added);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  ASSERT_TRUE(*Space);
  ASSERT_NE(Bytes, Original);

  Bytes = Original;
  std::optional<MachODependencyState> Slid;
  auto Published = reserveMachOPublishedUnwind(
      Bytes, Arch::X64, sizeof(uint32_t), Added, false, Slid,
      limits::backend::kMaxMachOHeaderGrowthBytes + 1);
  ASSERT_FALSE(bool(Published));
  llvm::consumeError(Published.takeError());
  EXPECT_EQ(Bytes, Original);
  EXPECT_FALSE(Slid);
}

TEST(SourcePatchLayout, MachOZeroNotePayloadIsNotUnusedHeaderSpace) {
  using namespace llvm::MachO;
  auto B = headerMachO();
  auto *H = reinterpret_cast<mach_header_64 *>(B.data());
  auto *N =
      reinterpret_cast<note_command *>(B.data() + sizeof(*H) + H->sizeofcmds);
  N->cmd = LC_NOTE;
  N->cmdsize = sizeof(*N);
  N->offset = 0x500;
  N->size = 128;
  H->sizeofcmds += sizeof(*N);
  ++H->ncmds;
  const auto Old = B;
  auto R = reserveMachOHeaderStorage(B, Arch::X64, 1024);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(B, Old);
}
TEST(SourcePatchLayout, MachORefusesUnmodeledInstructionCoverage) {
  using namespace llvm::MachO;
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto B = headerMachO();
    auto *H = reinterpret_cast<mach_header_64 *>(B.data());
    auto *S = reinterpret_cast<section_64 *>(B.data() + sizeof(*H) +
                                             2 * sizeof(segment_command_64));
    if (Case == 0)
      S->flags = S_ATTR_SOME_INSTRUCTIONS;
    if (Case == 1)
      S->flags = 0;
    if (Case == 2) {
      auto *D = reinterpret_cast<linkedit_data_command *>(
          B.data() + sizeof(*H) + H->sizeofcmds);
      D->cmd = LC_DATA_IN_CODE;
      D->cmdsize = sizeof(*D);
      D->dataoff = 0x2200;
      D->datasize = sizeof(data_in_code_entry);
      H->sizeofcmds += sizeof(*D);
      ++H->ncmds;
    }
    std::fill(B.begin() + sizeof(*H) + H->sizeofcmds, B.begin() + 0x800, 0x5a);
    const auto Old = B;
    auto R = reserveMachOHeaderStorage(B, Arch::X64, 512);
    ASSERT_FALSE(bool(R)) << Case;
    llvm::consumeError(R.takeError());
    EXPECT_EQ(B, Old);
  }
}
TEST(SourcePatchLayout, ChainedOrdinalsKeepPositiveHighBitValues) {
  using neverd::macho_loader::chainedLibraryOrdinal;
  EXPECT_EQ(chainedLibraryOrdinal(128, 256), 128);
  EXPECT_EQ(chainedLibraryOrdinal(240, 256), 240);
  EXPECT_EQ(chainedLibraryOrdinal(255, 256), -1);
  EXPECT_EQ(chainedLibraryOrdinal(32768, 65536), 32768);
  EXPECT_EQ(chainedLibraryOrdinal(65520, 65536), 65520);
  EXPECT_EQ(chainedLibraryOrdinal(65535, 65536), -1);
}

} // namespace
