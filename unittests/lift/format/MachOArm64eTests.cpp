//===- MachOArm64eTests.cpp - Apple authenticated pointer metadata -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently encoded dyld fixups distinguish analysis-time signing
/// recipes from materialized pointer values and test hostile chain bounds.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/RelocationProvenance.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

using namespace neverd;
using namespace llvm::MachO;

namespace {
constexpr va_t Base = 0x100000000;
constexpr va_t Data = Base + 0x1000;

template <typename T>
void put(std::vector<uint8_t> &Bytes, size_t Offset, const T &Value) {
  ASSERT_LE(Offset + sizeof(T), Bytes.size());
  std::memcpy(Bytes.data() + Offset, &Value, sizeof(T));
}

struct Chain {
  BinaryImage Image;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x400);
  macho_loader::ChainedFixupsInfo Info{0x100, 0x300};
  static constexpr size_t SegmentOffset = 0x128;
  Chain(uint16_t Format) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Base = Base;
    Image.MachOCPUSubtype = 0x80000002;
    Image.MachOHasChainedFixups = true;
    Image.DynInfo.NeededLibs = {"/usr/lib/libTest.dylib"};
    Image.MachODylibReferences = {{"/usr/lib/libTest.dylib", false}};
    Segment S;
    S.Name = "__DATA_CONST";
    S.VA = Data;
    S.Size = S.FileSz = 0x1000;
    S.Flags = SegmentFlags::Readable;
    S.Data.resize(S.Size);
    Image.Segments.push_back(S);
    Section Sec;
    Sec.Name = "__const";
    Sec.SegmentName = S.Name;
    Sec.VA = S.VA;
    Sec.Size = Sec.FileSz = S.Size;
    Sec.Flags = S.Flags;
    Image.Sections.push_back(Sec);
    dyld_chained_fixups_header H{};
    H.starts_offset = 0x20;
    H.imports_offset = 0x80;
    H.symbols_offset = 0xc0;
    H.imports_count = 2;
    H.imports_format = DYLD_CHAINED_IMPORT_ADDEND64;
    put(Bytes, Info.DataOff, H);
    writeLE<uint32_t>(Bytes.data() + 0x120, 1);
    writeLE<uint32_t>(Bytes.data() + 0x124, 8);
    dyld_chained_starts_in_segment Starts{};
    Starts.size = offsetof(dyld_chained_starts_in_segment, page_start) + 2;
    Starts.page_size = 0x1000;
    Starts.pointer_format = Format;
    Starts.segment_offset = 0x1000;
    Starts.page_count = 1;
    put(Bytes, SegmentOffset, Starts);
    dyld_chained_import_addend64 Import{};
    Import.lib_ordinal = 1;
    Import.name_offset = 0;
    Import.addend = 7;
    put(Bytes, 0x180, Import);
    Import.name_offset = 9;
    Import.addend = uint64_t(-4);
    put(Bytes, 0x190, Import);
    constexpr char Names[] = "_first__\0_second\0";
    std::memcpy(Bytes.data() + 0x1c0, Names, sizeof(Names));
  }
  void word(unsigned Offset, uint64_t Value) {
    writeLE<uint64_t>(Image.Segments[0].Data.data() + Offset, Value);
  }
  uint64_t word(unsigned Offset) const {
    return readLE<uint64_t>(Image.readVA(Data + Offset, 8));
  }
  void parse() {
    macho_loader::parseChainedFixupsImports(Bytes.data(), Bytes.size(), Info,
                                            Image);
    macho_loader::parseChainedFixupsRebases(Bytes.data(), Bytes.size(), Info,
                                            Base, Image);
  }
  std::vector<uint8_t> linkedFile() const {
    auto File = Bytes;
    File.resize(0x1400);
    mach_header_64 H{};
    H.magic = MH_MAGIC_64;
    H.cputype = CPU_TYPE_ARM64;
    H.cpusubtype = Image.MachOCPUSubtype;
    H.filetype = MH_BUNDLE;
    H.ncmds = 3;
    H.sizeofcmds =
        2 * sizeof(segment_command_64) + sizeof(linkedit_data_command);
    put(File, 0, H);
    segment_command_64 S{};
    S.cmd = LC_SEGMENT_64;
    S.cmdsize = sizeof(S);
    std::memcpy(S.segname, "__TEXT", 6);
    S.vmaddr = Base;
    S.vmsize = 0x1000;
    S.filesize = 0x400;
    S.initprot = S.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    put(File, sizeof(H), S);
    std::memset(S.segname, 0, sizeof(S.segname));
    std::memcpy(S.segname, "__DATA_CONST", 12);
    S.vmaddr = Data;
    S.fileoff = 0x400;
    S.filesize = 0x1000;
    S.initprot = S.maxprot = VM_PROT_READ;
    put(File, sizeof(H) + sizeof(S), S);
    linkedit_data_command Fixups{};
    Fixups.cmd = LC_DYLD_CHAINED_FIXUPS;
    Fixups.cmdsize = sizeof(Fixups);
    Fixups.dataoff = Info.DataOff;
    Fixups.datasize = Info.DataSize;
    put(File, sizeof(H) + 2 * sizeof(S), Fixups);
    // Add the __TEXT segment's empty start-table entry ahead of __DATA_CONST.
    std::memmove(File.data() + SegmentOffset + 4, File.data() + SegmentOffset,
                 24);
    writeLE<uint32_t>(File.data() + 0x120, 2);
    writeLE<uint32_t>(File.data() + 0x124, 0);
    writeLE<uint32_t>(File.data() + 0x128, 12);
    std::memcpy(File.data() + 0x400, Image.Segments[0].Data.data(), 0x1000);
    return File;
  }
};
} // namespace

TEST(MachOArm64e, RecognizesSubtypeWithABICapabilities) {
  for (uint32_t Subtype :
       {2u, 0x80000002u, 0x82000002u, 12u, 0x8000000cu, 0x8200000cu}) {
    mach_header_64 H{};
    H.magic = MH_MAGIC_64;
    H.cputype = CPU_TYPE_ARM64;
    H.cpusubtype = Subtype;
    H.filetype = MH_OBJECT;
    H.ncmds = 1;
    H.sizeofcmds = sizeof(segment_command_64) + sizeof(section_64);
    segment_command_64 S{};
    S.cmd = LC_SEGMENT_64;
    S.cmdsize = H.sizeofcmds;
    S.vmaddr = 0x1000;
    S.vmsize = S.filesize = 8;
    S.fileoff = sizeof(H) + H.sizeofcmds;
    S.nsects = 1;
    S.initprot = S.maxprot = VM_PROT_READ;
    section_64 Sec{};
    std::memcpy(Sec.sectname, "__const", 7);
    std::memcpy(Sec.segname, "__DATA_CONST", 12);
    Sec.addr = S.vmaddr;
    Sec.size = S.filesize;
    Sec.offset = S.fileoff;
    std::string Bytes(S.fileoff + S.filesize, '\0');
    std::memcpy(Bytes.data(), &H, sizeof(H));
    std::memcpy(Bytes.data() + sizeof(H), &S, sizeof(S));
    std::memcpy(Bytes.data() + sizeof(H) + sizeof(S), &Sec, sizeof(Sec));
    llvm::MemoryBufferRef Buffer(Bytes, "arm64e.o");
    MachOLoader Loader;
    auto Image = Loader.loadBuffer(Buffer);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_TRUE(Image->isArm64e());
    EXPECT_EQ(Image->MachOCPUSubtype, Subtype);
    std::vector<LoadCandidate> Candidates;
    Loader.identify(Buffer, Candidates);
    ASSERT_EQ(Candidates.size(), 1);
    EXPECT_TRUE(Candidates[0].Loadable);
    EXPECT_NE(Candidates[0].Description.find("ARM64E"), std::string::npos);
    if ((Subtype & 0x00ffffff) == 12)
      EXPECT_NE(Candidates[0].Description.find("ARM64E.X1"), std::string::npos);
  }
}

TEST(MachOArm64e, SegmentedChainsRetainTargetIndicesAndSigningFields) {
  for (bool Authenticated : {false, true})
    for (unsigned Key = 0; Key < 4; ++Key)
      for (bool AddressDiversity : {false, true}) {
        Chain F(14);
        // Keep the target out of address order, as its index refers to the
        // original load-command order. No image-base addition is involved.
        Segment Target;
        Target.VA = Base;
        Target.Size = 0x1000;
        F.Image.Segments.push_back(Target);
        const uint64_t Fields = (uint64_t(1) << 63) | (uint64_t(Key) << 49) |
                                (uint64_t(AddressDiversity) << 48) |
                                (uint64_t(0xbeef) << 32);
        const uint64_t Raw =
            (uint64_t(1) << 28) | 0x80 | (Authenticated ? Fields : 0);
        F.word(0, Raw);
        F.parse();
        ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty())
            << F.Image.MachOChainedFixupsError;
        if (Authenticated) {
          ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 1);
          const auto &A = F.Image.MachOAuthenticatedPointers.at(Data);
          EXPECT_EQ(A.Target, Base + 0x80);
          EXPECT_EQ(A.Key, Key);
          EXPECT_EQ(A.Diversity, 0xbeef);
          EXPECT_EQ(A.AddressDiversity, AddressDiversity);
          EXPECT_EQ(F.word(0), Raw);
          EXPECT_FALSE(readImmutableImagePointer(F.Image, Data));
        } else {
          EXPECT_TRUE(F.Image.MachOAuthenticatedPointers.empty());
          EXPECT_EQ(F.word(0), Base + 0x80);
          EXPECT_EQ(F.Image.MachOResolvedChainedPointerSlots.count(Data), 1);
        }
      }
}

TEST(MachOArm64e, SegmentedNextUsesAllTwelveBitsAtFourByteStride) {
  Chain F(14);
  F.Image.Segments[0].Size = F.Image.Segments[0].FileSz = 0x4000;
  F.Image.Segments[0].Data.resize(0x4000);
  F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 0x4000;
  writeLE<uint16_t>(F.Bytes.data() + Chain::SegmentOffset + 4, 0x4000);
  F.word(0, (uint64_t(1) << 63) | (uint64_t(0x800) << 51) | 0x80);
  F.word(0x2000, (uint64_t(1) << 63) | 0x88);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 2);
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.at(Data).Target, Data + 0x80);
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.at(Data + 0x2000).Target,
            Data + 0x88);
  EXPECT_TRUE(F.Image.DyldBindSlots.empty());
}

TEST(MachOArm64e, SegmentedMalformedTargetsNeverPublishPartialChains) {
  for (unsigned Failure = 0; Failure < 6; ++Failure) {
    SCOPED_TRACE(Failure);
    Chain F(14);
    F.word(0, (uint64_t(1) << 63) | (uint64_t(2) << 51) | 0x80);
    F.word(8, 0x88);
    if (Failure == 0)
      F.word(8, (uint64_t(15) << 28) | 0x88); // missing target segment
    else if (Failure == 1)
      F.word(8, 0x1001); // outside target segment, including its one-past value
    else if (Failure == 2)
      F.word(8, (uint64_t(1) << 32) | 0x88); // unsigned padding
    else if (Failure == 3) {
      Segment Target;
      Target.VA = InvalidVA - 8;
      Target.Size = 32;
      F.Image.Segments.push_back(Target);
      F.word(8, (uint64_t(1) << 28) | 16); // target address overflow
    } else if (Failure == 4)
      F.Image.IsRelocatable = true; // no load-command segment identity
    else
      F.Image.Segments.insert(F.Image.Segments.begin(), Segment{});
    F.parse();
    EXPECT_FALSE(F.Image.MachOChainedFixupsError.empty());
    EXPECT_TRUE(F.Image.MachOAuthenticatedPointers.empty());
    EXPECT_TRUE(F.Image.MachOResolvedChainedPointerSlots.empty());
  }
}

TEST(MachOArm64e, PublicLoaderAcceptsSegmentedPointersWithoutImageBaseBias) {
  for (bool Auth : {false, true}) {
    Chain F(14);
    // linkedFile adds __TEXT before __DATA_CONST in the segment table.
    const uint64_t Raw = (uint64_t(Auth) << 63) | (uint64_t(1) << 28) | 0x80;
    F.word(0, Raw);
    const auto File = F.linkedFile();
    llvm::MemoryBufferRef Buffer(
        llvm::StringRef(reinterpret_cast<const char *>(File.data()),
                        File.size()),
        "segmented-arm64e.dylib");
    auto Image = MachOLoader().loadBuffer(Buffer);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    if (Auth) {
      ASSERT_EQ(Image->MachOAuthenticatedPointers.size(), 1);
      EXPECT_EQ(Image->MachOAuthenticatedPointers.at(Data).Target, Data + 0x80);
      EXPECT_EQ(readLE<uint64_t>(Image->readVA(Data, 8)), Raw);
    } else {
      EXPECT_EQ(readLE<uint64_t>(Image->readVA(Data, 8)), Data + 0x80);
      EXPECT_EQ(Image->MachOResolvedChainedPointerSlots.count(Data), 1);
    }
  }
}

TEST(MachOArm64e, RetainsAllSigningFieldsWithoutInventingPACBits) {
  for (uint16_t Format :
       {DYLD_CHAINED_PTR_ARM64E, DYLD_CHAINED_PTR_ARM64E_KERNEL,
        DYLD_CHAINED_PTR_ARM64E_USERLAND, DYLD_CHAINED_PTR_ARM64E_FIRMWARE,
        DYLD_CHAINED_PTR_ARM64E_USERLAND24})
    for (unsigned Key = 0; Key < 4; ++Key)
      for (bool AddressDiversity : {false, true}) {
        Chain F(Format);
        const uint64_t Raw = (uint64_t(1) << 63) | (uint64_t(Key) << 49) |
                             (uint64_t(AddressDiversity) << 48) |
                             (uint64_t(0xbeef) << 32) | 0x1080;
        F.word(0, Raw);
        F.parse();
        ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
        ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 1);
        const auto &Auth = F.Image.MachOAuthenticatedPointers.at(Data);
        EXPECT_EQ(Auth.Target, Data + 0x80);
        EXPECT_EQ(Auth.Key, Key);
        EXPECT_EQ(Auth.Diversity, 0xbeef);
        EXPECT_EQ(Auth.AddressDiversity, AddressDiversity);
        EXPECT_EQ(F.word(0), Raw);
        EXPECT_TRUE(F.Image.MachOResolvedChainedPointerSlots.empty());
        EXPECT_TRUE(F.Image.CodePtrRelocSlots.empty());
        EXPECT_TRUE(F.Image.DataPtrRelocSlots.empty());
        EXPECT_TRUE(F.Image.hasRelocationProvenanceAt(Data));
        EXPECT_TRUE(hasRelocationProvenanceIn(F.Image, Data, Data + 8));
        EXPECT_FALSE(readImmutableImageBytes(F.Image, Data + 4, 4));
        EXPECT_FALSE(readImmutableImagePointer(F.Image, Data));
        EXPECT_FALSE(readImmutableChainedImageValue(F.Image, Data));
      }
}

TEST(MachOArm64e, X1CannotBypassAuthenticatedABIRebuildRequirements) {
  for (uint32_t Subtype : {0u, 12u, 0x8200000cu}) {
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOCPUSubtype = Subtype;
    if (!Subtype)
      Image.MachOSharedCache =
          std::make_shared<macho_loader::DyldSharedCache>();
    MedFunc F;
    F.Name = "x1_constant";
    F.Entry = Base;
    F.ReturnType = NdType::makeInt(8, false);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(MedVar::makeConst(7, 8));
    F.Blocks.emplace_back();
    F.Blocks[0].Id = 0;
    F.Blocks[0].StartAddr = Base;
    F.Blocks[0].Ops = {Return};
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({F}, Context, "x1", Arch::AArch64, {},
                                        &Image, BinaryFormat::MachO);
    ASSERT_TRUE(Module);
    EXPECT_TRUE(Module->getModuleFlag(Subtype ? "neverd.arm64e-analysis"
                                              : "neverd.dyld-cache-analysis"));
    EXPECT_FALSE(
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO).Success);
    auto Replacement = sourcePatchTargetTriple(Image);
    ASSERT_FALSE(bool(Replacement));
    llvm::consumeError(Replacement.takeError());
  }
}

TEST(MachOArm64e, MixedChainsUseFormatStrideAndSignedBindAddends) {
  for (uint16_t Format :
       {DYLD_CHAINED_PTR_ARM64E, DYLD_CHAINED_PTR_ARM64E_KERNEL,
        DYLD_CHAINED_PTR_ARM64E_USERLAND, DYLD_CHAINED_PTR_ARM64E_FIRMWARE,
        DYLD_CHAINED_PTR_ARM64E_USERLAND24}) {
    Chain F(Format);
    const unsigned Next = Format == DYLD_CHAINED_PTR_ARM64E_KERNEL ||
                                  Format == DYLD_CHAINED_PTR_ARM64E_FIRMWARE
                              ? 2
                              : 1;
    F.word(0, (uint64_t(1) << 63) | (uint64_t(1) << 62) |
                  (uint64_t(Next) << 51) | (uint64_t(0xabcd) << 32) | 1);
    F.word(8, (uint64_t(1) << 62) | (uint64_t(Next) << 51) |
                  (uint64_t(0x7fffd) << 32)); // signed inline addend -3
    const bool Absolute = Format == DYLD_CHAINED_PTR_ARM64E ||
                          Format == DYLD_CHAINED_PTR_ARM64E_FIRMWARE;
    F.word(16, Absolute ? Data + 0x80 : 0x1080);
    F.parse();
    ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
    const auto &Auth = F.Image.MachOAuthenticatedPointers.at(Data);
    EXPECT_EQ(Auth.Symbol, "_second");
    EXPECT_EQ(Auth.Module, "/usr/lib/libTest.dylib");
    EXPECT_EQ(Auth.Addend, -4);
    EXPECT_EQ(Auth.Diversity, 0xabcd);
    EXPECT_EQ(Auth.Target, InvalidVA);
    EXPECT_EQ(F.Image.DyldBindSlots.count(Data), 0);
    EXPECT_FALSE(F.Image.isValidImportStorageSlot(Data, Auth.Symbol));
    ASSERT_EQ(F.Image.DyldBindSlots.count(Data + 8), 1);
    EXPECT_EQ(F.Image.DyldBindSlots.at(Data + 8).Addend, 4);
    EXPECT_EQ(F.word(16), Data + 0x80);
    EXPECT_EQ(F.Image.MachOResolvedChainedPointerSlots.count(Data + 16), 1);
  }
}

TEST(MachOArm64e, PublicLoaderAcceptsSigningRecipesAndRejectsUnknownLayouts) {
  for (uint16_t Format :
       {uint16_t(DYLD_CHAINED_PTR_ARM64E_USERLAND24), uint16_t(13)}) {
    Chain F(Format);
    F.word(0, (uint64_t(1) << 63) | 0x1080);
    const auto File = F.linkedFile();
    llvm::MemoryBufferRef Buffer(
        llvm::StringRef(reinterpret_cast<const char *>(File.data()),
                        File.size()),
        "arm64e.dylib");
    auto Image = MachOLoader().loadBuffer(Buffer);
    if (Format == 13) {
      ASSERT_FALSE(bool(Image));
      EXPECT_NE(llvm::toString(Image.takeError()).find("unsupported arm64e"),
                std::string::npos);
    } else {
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      EXPECT_TRUE(Image->isArm64e());
      ASSERT_EQ(Image->MachOAuthenticatedPointers.size(), 1);
      EXPECT_EQ(Image->MachOAuthenticatedPointers.at(Data).Target, Data + 0x80);
    }
  }
}

TEST(MachOArm64e, MultipleStartsAndHighTagsStayDistinct) {
  Chain F(DYLD_CHAINED_PTR_ARM64E_USERLAND);
  auto *Starts = F.Bytes.data() + Chain::SegmentOffset;
  writeLE<uint32_t>(Starts, 28);
  writeLE<uint16_t>(Starts + 22, DYLD_CHAINED_PTR_START_MULTI | 1);
  writeLE<uint16_t>(Starts + 24, 0);
  writeLE<uint16_t>(Starts + 26, DYLD_CHAINED_PTR_START_LAST | 16);
  F.word(0, (uint64_t(1) << 63) | 0x1080);
  F.word(16, (uint64_t(0xab) << 43) | 0x1080);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.size(), 1);
  EXPECT_EQ(F.word(16), (uint64_t(0xab) << 56) | (Data + 0x80));
  EXPECT_EQ(F.Image.MachOResolvedChainedPointerSlots.count(Data + 16), 1);
  EXPECT_EQ(F.Image.DataPtrRelocSlots.count(Data + 16), 0);
}

TEST(MachOArm64e, MalformedHeadersAndSegmentOwnershipFailClearly) {
  for (unsigned Failure = 0; Failure < 4; ++Failure) {
    Chain F(DYLD_CHAINED_PTR_ARM64E_USERLAND);
    if (Failure == 0)
      F.Info.DataSize = 8;
    else if (Failure == 1)
      writeLE<uint32_t>(F.Bytes.data() + F.Info.DataOff, 1);
    else if (Failure == 2)
      writeLE<uint64_t>(F.Bytes.data() + Chain::SegmentOffset + 8, 0x1080);
    else
      F.Image.MachOChainedFixupsAmbiguous = true;
    F.parse();
    EXPECT_FALSE(F.Image.MachOChainedFixupsError.empty());
    EXPECT_TRUE(F.Image.MachOAuthenticatedPointers.empty());
  }
}

TEST(MachOArm64e,
     SourceKeepsSignedStorageExternalAndNativeRebuildFailsClearly) {
  for (auto [Offset, Width] : {std::pair{0u, 8u}, {4u, 4u}, {0u, 16u}}) {
    SCOPED_TRACE(Width);
    Chain Fixture(DYLD_CHAINED_PTR_ARM64E_USERLAND);
    Fixture.word(0, (uint64_t(1) << 63) | (uint64_t(0xabcd) << 32) | 0x1080);
    Fixture.parse();
    ASSERT_TRUE(Fixture.Image.MachOChainedFixupsError.empty());
    ASSERT_TRUE(
        Fixture.Image.hasMachOAuthenticatedPointerIn(Data + Offset, Width));
    MedFunc F;
    F.Name = "read_signed_storage";
    F.Entry = Base;
    F.ReturnType = NdType::makeInt(Width, false);
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output.Kind = MedVar::Reg;
    Load.Output.TheArch = Arch::AArch64;
    Load.Output.Size = Width;
    Load.Output.RegOff = a64reg::X0;
    Load.Output.Id = Load.Output.SSAVer = 1;
    Load.addInput(MedVar::makeConst(Data + Offset, 8));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Load.Output);
    F.Blocks.emplace_back();
    F.Blocks[0].Id = 0;
    F.Blocks[0].StartAddr = F.Entry;
    F.Blocks[0].Ops = {Load, Return};
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({F}, Context, "signed-storage", Arch::AArch64, {},
                              &Fixture.Image, BinaryFormat::MachO);
    ASSERT_TRUE(Module);
    unsigned Globals = 0;
    for (const auto &Global : Module->globals()) {
      EXPECT_FALSE(Global.hasInitializer()) << Global.getName().str();
      ++Globals;
    }
    EXPECT_GT(Globals, 0);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    Options.Image = &Fixture.Image;
    std::string LowC, HighC;
    llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
    EXPECT_TRUE(LLVMCEmitter().emit(*Module, LowOS, Options));
    auto High = MedToHighConverter().convert(F, Arch::AArch64);
    EXPECT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
    for (const std::string *C : {&LowC, &HighC}) {
      EXPECT_NE(C->find("extern "), std::string::npos) << *C;
      EXPECT_EQ(C->find("8000ABCD00001080"), std::string::npos) << *C;
      EXPECT_EQ(C->find("8000abcd00001080"), std::string::npos) << *C;
    }
    EXPECT_FALSE(
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO).Success);
  }
}

TEST(MachOArm64e, RejectsMalformedChainsWithoutPublishingTheirPrefix) {
  for (unsigned Failure = 0; Failure < 5; ++Failure) {
    Chain F(DYLD_CHAINED_PTR_ARM64E_USERLAND24);
    F.word(0, (uint64_t(1) << 63) | (uint64_t(1) << 51) | 0x1080);
    F.word(8, (uint64_t(1) << 62) | 1);
    if (Failure == 0)
      F.word(8, (uint64_t(1) << 62) | 0x10001); // full 24-bit ordinal
    else if (Failure == 1)
      F.word(8, (uint64_t(1) << 62) | (uint64_t(1) << 24)); // reserved bits
    else if (Failure == 2)
      F.word(8, (uint64_t(0x7ff) << 51) | 0x1080); // leaves page
    else if (Failure == 3)
      writeLE<uint16_t>(
          F.Bytes.data() + Chain::SegmentOffset +
              offsetof(dyld_chained_starts_in_segment, page_start),
          0xffc);
    else
      writeLE<uint16_t>(F.Bytes.data() + Chain::SegmentOffset +
                            offsetof(dyld_chained_starts_in_segment, page_size),
                        0);
    F.parse();
    EXPECT_FALSE(F.Image.MachOChainedFixupsError.empty());
    EXPECT_TRUE(F.Image.MachOAuthenticatedPointers.empty());
    EXPECT_TRUE(F.Image.MachOResolvedChainedPointerSlots.empty());
    EXPECT_TRUE(F.Image.DyldBindSlots.empty());
  }
}

TEST(MachOArm64e, MetadataTargetsRequireUnchangedUnambiguousLocalRecipes) {
  using macho_loader::readMetadataPointerTarget;
  for (uint16_t Format : {1, 7, 9, 10, 12, 14}) {
    Chain Fixture(Format);
    // Segmented format 14 uses this fixture's segment zero + 0x80.
    const uint64_t Raw = 0x8007432100000000ULL | (Format == 14 ? 0x80 : 0x1080);
    Fixture.word(0, Raw);
    Fixture.parse();
    ASSERT_TRUE(Fixture.Image.MachOChainedFixupsError.empty());
    ASSERT_EQ(Fixture.Image.MachOAuthenticatedPointers.size(), 1);
    EXPECT_EQ(Fixture.Image.MachOAuthenticatedPointers.at(Data).EncodedValue,
              Raw);
    EXPECT_EQ(readMetadataPointerTarget(Fixture.Image, Data), Data + 0x80);
    EXPECT_EQ(Fixture.word(0), Raw);
    for (unsigned Invalid = 0; Invalid < 18; ++Invalid) {
      SCOPED_TRACE(testing::Message() << Format << " " << Invalid);
      auto Image = Fixture.Image;
      auto &Recipe = Image.MachOAuthenticatedPointers.at(Data);
      switch (Invalid) {
      case 0:
        Recipe.EncodedValue.reset();
        break;
      case 1:
        Image.Segments[0].Data[0] ^= 1;
        break;
      case 2:
        Recipe.Key = 1;
        break;
      case 3:
        Recipe.Diversity ^= 1;
        break;
      case 4:
        Recipe.AddressDiversity = false;
        break;
      case 5:
        Recipe.Target = InvalidVA;
        Recipe.Symbol = "_imported";
        break;
      case 6:
        Recipe.Target = Data + 0x2000;
        break;
      case 7:
        Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 8:
        Image.MachOResolvedChainedPointerSlots.insert(Data);
        break;
      case 9:
        Image.MachOAuthenticatedPointers.emplace(Data + 4, Recipe);
        break;
      case 10:
        Image.DyldBindSlots[Data - 4] = {"_conflict", 0};
        break;
      case 11:
        Image.ConflictingImportStorageSlots.insert(Data);
        break;
      case 12:
        Image.Segments.push_back(Image.Segments[0]);
        break;
      case 13:
        Image.Segments[0].FileSz = 4;
        break;
      case 14:
      case 15: {
        Segment Target = Image.Segments[0];
        Target.VA = Recipe.Target;
        Image.Segments.push_back(std::move(Target));
        if (Invalid == 14)
          Image.Segments[0].Size = 4;
        break;
      }
      case 16:
        Image.Segments[0].Size = InvalidVA - Data + 1;
        break;
      case 17:
        Image.Segments[0].FileSz = 0x80;
        break;
      }
      EXPECT_FALSE(readMetadataPointerTarget(Image, Data));
    }
    for (unsigned Offset = 1; Offset < 8; ++Offset)
      EXPECT_FALSE(readMetadataPointerTarget(Fixture.Image, Data + Offset));
  }
}

TEST(MachOArm64e, AuthenticatedInitializersAndFinalizersAreDiscovered) {
  for (uint32_t Kind : {S_MOD_INIT_FUNC_POINTERS, S_MOD_TERM_FUNC_POINTERS,
                        S_THREAD_LOCAL_INIT_FUNCTION_POINTERS}) {
    Chain Fixture(DYLD_CHAINED_PTR_ARM64E_USERLAND);
    const uint64_t Raw = 0x8000000000001080ULL;
    Fixture.word(0, Raw);
    Fixture.Image.Segments[0].Flags =
        SegmentFlags::Readable | SegmentFlags::Executable;
    Fixture.Image.Sections[0].Size = Fixture.Image.Sections[0].FileSz = 8;
    neverd::Section Text = Fixture.Image.Sections[0];
    Text.Name = "__text";
    Text.VA += 0x80;
    Text.FileOff += 0x80;
    Text.Size = Text.FileSz = 4;
    Text.Type = S_ATTR_PURE_INSTRUCTIONS;
    Text.Flags = Fixture.Image.Segments[0].Flags;
    Fixture.Image.Sections.push_back(Text);
    Fixture.word(0x80, 0xd65f03c0); // ret
    Fixture.parse();
    ASSERT_TRUE(Fixture.Image.MachOChainedFixupsError.empty());
    ASSERT_EQ(macho_loader::readMetadataPointerTarget(Fixture.Image, Data),
              Data + 0x80);
    macho_loader::SectionInfo Section{};
    Section.Addr = Data;
    Section.Size = 8;
    Section.Flags = Kind;
    macho_loader::parseRuntimeFunctionSections({Section}, Base, Fixture.Image);
    const auto &Functions = Kind == S_MOD_TERM_FUNC_POINTERS
                                ? Fixture.Image.DynInfo.FiniArray
                                : Fixture.Image.DynInfo.InitArray;
    ASSERT_EQ(Functions.size(), 1);
    EXPECT_EQ(Functions[0], Data + 0x80);
    EXPECT_EQ(Fixture.word(0), Raw);
    EXPECT_TRUE(Fixture.Image.MachOResolvedChainedPointerSlots.empty());
    Fixture.Image.DynInfo.InitArray.clear();
    Fixture.Image.DynInfo.FiniArray.clear();
    Fixture.word(0, Data + 0x80); // Editing bytes invalidates the recipe.
    macho_loader::parseRuntimeFunctionSections({Section}, Base, Fixture.Image);
    EXPECT_TRUE(Fixture.Image.DynInfo.InitArray.empty());
    EXPECT_TRUE(Fixture.Image.DynInfo.FiniArray.empty());
  }
}
