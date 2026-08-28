//===- RuntimeMetadataTests.cpp - Cross-format runtime metadata tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace neverd;

Segment makeSegment(va_t VA, uint64_t Size, bool Executable) {
  Segment Seg;
  Seg.VA = VA;
  Seg.Size = Size;
  Seg.Flags = SegmentFlags::Readable;
  if (Executable)
    Seg.Flags = Seg.Flags | SegmentFlags::Executable;
  Seg.Data.resize(static_cast<size_t>(Size));
  return Seg;
}

struct DelayImportImage {
  BinaryImage Img;
  llvm::object::delay_import_directory_table_entry Desc{};
};

DelayImportImage makeDelayImportImage(bool Is64, bool LegacyVA,
                                      bool ByOrdinal) {
  constexpr va_t ImageBase = 0x400000;
  constexpr uint32_t SegmentRVA = 0x1000;
  constexpr uint32_t IATRVA = 0x1100;
  constexpr uint32_t INTRVA = 0x1200;
  constexpr uint32_t HintNameRVA = 0x1300;

  DelayImportImage Fixture;
  Fixture.Img.Base = ImageBase;
  Fixture.Img.Bits = Is64 ? Bitness::Bits64 : Bitness::Bits32;
  Fixture.Img.Segments.push_back(
      makeSegment(ImageBase + SegmentRVA, 0x400, false));
  Segment &Seg = Fixture.Img.Segments.back();

  const char Module[] = "example.dll";
  std::memcpy(Seg.Data.data(), Module, sizeof(Module));
  const char Name[] = "delayed_function";
  std::memcpy(Seg.Data.data() + (HintNameRVA - SegmentRVA) + sizeof(uint16_t),
              Name, sizeof(Name));

  const uint32_t PtrSize = Fixture.Img.getPointerSize();
  uint64_t Lookup = 0;
  if (ByOrdinal)
    Lookup = (Is64 ? uint64_t(1) << 63 : uint64_t(1) << 31) | 37;
  else
    Lookup = LegacyVA ? ImageBase + HintNameRVA : HintNameRVA;
  writePtr(Seg.Data.data() + (INTRVA - SegmentRVA), Lookup, Is64);
  writePtr(Seg.Data.data() + (IATRVA - SegmentRVA), 0xfeedface, Is64);
  writePtr(Seg.Data.data() + (INTRVA - SegmentRVA) + PtrSize, 0, Is64);
  writePtr(Seg.Data.data() + (IATRVA - SegmentRVA) + PtrSize, 0, Is64);

  auto Encode = [&](uint32_t RVA) -> uint32_t {
    return LegacyVA ? static_cast<uint32_t>(ImageBase + RVA) : RVA;
  };
  Fixture.Desc.Attributes = 0;
  Fixture.Desc.Name = Encode(SegmentRVA);
  Fixture.Desc.DelayImportAddressTable = Encode(IATRVA);
  Fixture.Desc.DelayImportNameTable = Encode(INTRVA);
  return Fixture;
}

template <typename T> std::vector<uint8_t> objectBytes(const T &Object) {
  std::vector<uint8_t> Bytes(sizeof(Object));
  std::memcpy(Bytes.data(), &Object, sizeof(Object));
  return Bytes;
}

std::vector<uint8_t> makeThreadCommand(uint32_t Type, va_t Entry) {
  using namespace llvm::MachO;
  x86_thread_state64_t State{};
  State.rip = Entry;

  thread_command Command{};
  Command.cmd = Type;
  Command.cmdsize = static_cast<uint32_t>(sizeof(Command) +
                                          2 * sizeof(uint32_t) + sizeof(State));
  std::vector<uint8_t> Bytes = objectBytes(Command);
  size_t Offset = Bytes.size();
  Bytes.resize(Offset + 2 * sizeof(uint32_t) + sizeof(State));
  writeLE<uint32_t>(Bytes.data() + Offset, x86_THREAD_STATE64);
  writeLE<uint32_t>(Bytes.data() + Offset + sizeof(uint32_t),
                    x86_THREAD_STATE64_COUNT);
  std::memcpy(Bytes.data() + Offset + 2 * sizeof(uint32_t), &State,
              sizeof(State));
  return Bytes;
}

std::vector<uint8_t>
makeMachOImage(const std::vector<std::vector<uint8_t>> &Commands) {
  using namespace llvm::MachO;
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = CPU_TYPE_X86_64;
  Header.cpusubtype = CPU_SUBTYPE_X86_64_ALL;
  Header.filetype = MH_EXECUTE;
  Header.ncmds = static_cast<uint32_t>(Commands.size());
  for (const auto &Command : Commands)
    Header.sizeofcmds += static_cast<uint32_t>(Command.size());

  std::vector<uint8_t> Bytes = objectBytes(Header);
  for (const auto &Command : Commands)
    Bytes.insert(Bytes.end(), Command.begin(), Command.end());
  return Bytes;
}

std::unique_ptr<llvm::object::MachOObjectFile>
createMachOObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Data, "RuntimeMetadata Mach-O fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

TEST(RuntimeMetadata, KeepsNativeIATAddressWhenRecordingStub) {
  BinaryImage Img;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));

  Import Imp;
  Imp.Module = "example.dll";
  Imp.Name = "imported";
  Imp.IATAddr = 0x3020;
  Img.Imports.push_back(Imp);

  ASSERT_TRUE(Img.recordImportStub(0x1010, 0));
  EXPECT_EQ(Img.Imports[0].IATAddr, 0x3020u);
  EXPECT_EQ(Img.findImportAt(0x3020), &Img.Imports[0]);
  EXPECT_EQ(Img.findImportAt(0x1010), &Img.Imports[0]);
  EXPECT_TRUE(Img.isImportStubAt(0x1010));
  EXPECT_FALSE(Img.isImportStubAt(0x3020));

  auto Names = Img.getImportAddressNames();
  EXPECT_EQ(Names[0x3020], "imported");
  EXPECT_EQ(Names[0x1010], "imported");
}

TEST(RuntimeMetadata, RecordsOnlyExactMappedImportStorageSlots) {
  BinaryImage Img;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));
  Img.Segments.push_back(makeSegment(0x5000, 0x100, false));
  Img.Segments.back().Data.resize(4);

  EXPECT_TRUE(Img.recordImportStorageSlot(
      0x3020, "imported", 0,
      ImportStorageEvidence::ImportDirectory));
  ASSERT_EQ(Img.ImportStorageSlots.size(), 1u);
  EXPECT_EQ(Img.ImportStorageSlots.at(0x3020).Name, "imported");
  EXPECT_EQ(Img.ImportStorageSlots.at(0x3020).Addend, 0);
  EXPECT_TRUE(Img.hasRelocationProvenanceAt(0x3020));
  EXPECT_FALSE(recordAbsolutePointerRelocation(Img, 0x3020, 0x1010));
  EXPECT_TRUE(Img.CodePtrRelocSlots.empty());

  EXPECT_FALSE(Img.recordImportStorageSlot(
      0x3021, "unaligned", 0,
      ImportStorageEvidence::ImportDirectory));
  EXPECT_FALSE(Img.recordImportStorageSlot(
      0x1010, "executable", 0,
      ImportStorageEvidence::ImportDirectory));
  EXPECT_FALSE(Img.recordImportStorageSlot(
      0x4000, "unmapped", 0,
      ImportStorageEvidence::ImportDirectory));
  EXPECT_FALSE(Img.recordImportStorageSlot(
      0x5000, "truncated", 0,
      ImportStorageEvidence::ImportDirectory));
  EXPECT_FALSE(Img.recordImportStorageSlot(
      0x3028, "", 0, ImportStorageEvidence::ImportDirectory));
  EXPECT_EQ(Img.ImportStorageSlots.size(), 1u);

  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = 0x3020;
  Img.Imports.push_back(std::move(Imp));
  ASSERT_TRUE(Img.recordImportStub(0x1010, 0));
  EXPECT_TRUE(Img.isImportStubAt(0x1010));
  EXPECT_EQ(Img.ImportStorageSlots.count(0x1010), 0u)
      << "an executable import veneer must not become pointer storage";

  // Legacy format-native maps are normalized through the same validator; a
  // direct-map fixture cannot turn an executable stub into storage either.
  Img.ImportPtrSlots[0x1010] = "legacy_veneer";
  const ImportStorageSlotCollection Collected =
      Img.collectImportStorageSlots();
  EXPECT_EQ(Collected.Slots.count(0x1010), 0u);
  EXPECT_FALSE(Img.hasRelocationProvenanceAt(0x1010));
}

TEST(RuntimeMetadata, ConflictingImportStorageIdentityFailsClosed) {
  BinaryImage Img;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));

  ASSERT_TRUE(Img.recordImportStorageSlot(
      0x3020, "first", 0, ImportStorageEvidence::ImportDirectory));
  EXPECT_TRUE(Img.recordImportStorageSlot(
      0x3020, "first", 0, ImportStorageEvidence::ImportDirectory));
  EXPECT_FALSE(Img.recordImportStorageSlot(
      0x3020, "second", 0, ImportStorageEvidence::ImportDirectory));
  EXPECT_EQ(Img.ImportStorageSlots.count(0x3020), 0u);
  EXPECT_TRUE(Img.ConflictingImportStorageSlots.count(0x3020));
  EXPECT_TRUE(Img.hasPointerStorageProvenanceAt(0x3020));
}

TEST(RuntimeMetadata, NormalizesNativeImportStorageWithoutFirstWins) {
  BinaryImage Img;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));

  Img.ImportPtrSlots[0x3020] = "same";
  Img.DyldBindSlots[0x3020] = {"same", 0};
  ImportStorageSlotCollection Collected = Img.collectImportStorageSlots();
  ASSERT_EQ(Collected.Slots.count(0x3020), 1u);
  EXPECT_EQ(Collected.Slots.at(0x3020).Name, "same");
  EXPECT_EQ(Collected.Slots.at(0x3020).Addend, 0);
  EXPECT_EQ(Collected.Slots.at(0x3020).Evidence,
            ImportStorageEvidence::LoaderBind);
  EXPECT_TRUE(Collected.Conflicts.empty());

  Img.DyldBindSlots[0x3020] = {"same", 12};
  Collected = Img.collectImportStorageSlots();
  ASSERT_EQ(Collected.Slots.count(0x3020), 1u);
  EXPECT_EQ(Collected.Slots.at(0x3020).Addend, 12);

  Img.DyldBindSlots[0x3020] = {"different", 12};
  Collected = Img.collectImportStorageSlots();
  EXPECT_EQ(Collected.Slots.count(0x3020), 0u);
  EXPECT_TRUE(Collected.Conflicts.count(0x3020));
}

TEST(RuntimeMetadata, TextNormalizationKeepsImportStorageViewsConsistent) {
  BinaryImage Img;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));
  const std::string InvalidName(1, static_cast<char>(0xff));
  ASSERT_TRUE(Img.recordImportStorageSlot(
      0x3020, InvalidName, 0, ImportStorageEvidence::PointerTable));
  Img.ImportPtrSlots[0x3020] = InvalidName;

  normalizeBinaryMetadata(Img);
  ASSERT_EQ(Img.ImportStorageSlots.count(0x3020), 1u);
  EXPECT_EQ(Img.ImportStorageSlots.at(0x3020).Name,
            Img.ImportPtrSlots.at(0x3020));
  const ImportStorageSlotCollection Collected =
      Img.collectImportStorageSlots();
  EXPECT_EQ(Collected.Slots.count(0x3020), 1u);
  EXPECT_TRUE(Collected.Conflicts.empty());
}

TEST(RuntimeMetadata, LegacyMachOIATStubFallbackRespectsSectionCodeAuthority) {
  constexpr va_t LegacyIATAddr = 0x1010;

  BinaryImage Img;
  Img.Format = BinaryFormat::MachO;

  Segment Text = makeSegment(0x1000, 0x100, true);
  Text.Name = "__TEXT";
  Img.Segments.push_back(std::move(Text));

  Section Const;
  Const.Name = "__const";
  Const.SegmentName = "__TEXT";
  Const.VA = 0x1000;
  Const.Size = 0x40;
  Const.FileSz = Const.Size;
  Const.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Const.Type = llvm::MachO::S_REGULAR;
  Img.Sections.push_back(std::move(Const));

  Import Imp;
  Imp.Name = "legacy_macho_import";
  Imp.IATAddr = LegacyIATAddr;
  Img.Imports.push_back(std::move(Imp));

  EXPECT_FALSE(Img.isImportStubAt(LegacyIATAddr));

  BinaryImage Sectionless = Img;
  Sectionless.Sections.clear();
  EXPECT_TRUE(Sectionless.isImportStubAt(LegacyIATAddr));

  BinaryImage RangeBacked = Img;
  ASSERT_TRUE(RangeBacked.recordImportStubRange(LegacyIATAddr, 8));
  EXPECT_TRUE(RangeBacked.isImportStubAt(LegacyIATAddr));

  ASSERT_TRUE(Img.recordImportStub(LegacyIATAddr, 0));
  EXPECT_TRUE(Img.isImportStubAt(LegacyIATAddr));

  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(Format == BinaryFormat::ELF ? "ELF" : "COFF");

    BinaryImage Native;
    Native.Format = Format;
    Native.Segments.push_back(makeSegment(0x1000, 0x100, true));

    Import NativeImport;
    NativeImport.Name = "native_import";
    NativeImport.IATAddr = LegacyIATAddr;
    Native.Imports.push_back(std::move(NativeImport));

    EXPECT_FALSE(Native.isImportStubAt(LegacyIATAddr));
    ASSERT_TRUE(Native.recordImportStub(LegacyIATAddr, 0));
    EXPECT_TRUE(Native.isImportStubAt(LegacyIATAddr));
  }
}

TEST(BinaryLoading, NormalizesEveryExternalMetadataField) {
  auto Malformed = [](llvm::StringRef Prefix) {
    std::string Value = Prefix.str();
    Value.push_back(static_cast<char>(0xff));
    return Value;
  };
  auto Escaped = [](llvm::StringRef Prefix) {
    return (Prefix + "\\xFF").str();
  };

  BinaryImage Img;
  Img.Segments.push_back(Segment{});
  Img.Segments.back().Name = Malformed("segment");
  Img.Sections.push_back(Section{});
  Img.Sections.back().Name = Malformed("section");
  Img.Sections.back().SegmentName = Malformed("section-segment");
  Img.Imports.push_back(Import{});
  Img.Imports.back().Module = Malformed("module");
  Img.Imports.back().Name = Malformed("import");
  Img.Exports.push_back(Export{});
  Img.Exports.back().Name = Malformed("export");
  Img.Symbols.push_back(Symbol{});
  Img.Symbols.back().Name = Malformed("symbol");
  Img.Relocations.push_back(RelocationEntry{});
  Img.Relocations.back().SymbolName = Malformed("reloc-symbol");
  Img.Relocations.back().SectionName = Malformed("reloc-section");
  Img.ImportPtrSlots[0x1000] = Malformed("pointer-slot");
  Img.DynInfo.SOName = Malformed("soname");
  Img.DynInfo.NeededLibs.push_back(Malformed("needed"));
  Img.DynInfo.RPaths.push_back(Malformed("rpath"));
  Img.DynInfo.PDBPath = Malformed("debug-path");
  Img.DynInfo.UUID = Malformed("uuid");
  Img.DynInfo.MinOSVersion = Malformed("minimum-os");
  Img.ExceptionMetadata.Diagnostics.push_back(Malformed("image-diagnostic"));
  Img.ExceptionMetadata.Functions.push_back(ExceptionFunction{});
  Img.ExceptionMetadata.Functions.back().PersonalityName =
      Malformed("personality");
  Img.ExceptionMetadata.Functions.back().Diagnostics.push_back(
      Malformed("function-diagnostic"));

  normalizeBinaryMetadata(Img);

  EXPECT_EQ(Img.Segments[0].Name, Escaped("segment"));
  EXPECT_EQ(Img.Sections[0].Name, Escaped("section"));
  EXPECT_EQ(Img.Sections[0].SegmentName, Escaped("section-segment"));
  EXPECT_EQ(Img.Imports[0].Module, Escaped("module"));
  EXPECT_EQ(Img.Imports[0].Name, Escaped("import"));
  EXPECT_EQ(Img.Exports[0].Name, Escaped("export"));
  EXPECT_EQ(Img.Symbols[0].Name, Escaped("symbol"));
  EXPECT_EQ(Img.Relocations[0].SymbolName, Escaped("reloc-symbol"));
  EXPECT_EQ(Img.Relocations[0].SectionName, Escaped("reloc-section"));
  EXPECT_EQ(Img.ImportPtrSlots.at(0x1000), Escaped("pointer-slot"));
  EXPECT_EQ(Img.DynInfo.SOName, Escaped("soname"));
  EXPECT_EQ(Img.DynInfo.NeededLibs[0], Escaped("needed"));
  EXPECT_EQ(Img.DynInfo.RPaths[0], Escaped("rpath"));
  EXPECT_EQ(Img.DynInfo.PDBPath, Escaped("debug-path"));
  EXPECT_EQ(Img.DynInfo.UUID, Escaped("uuid"));
  EXPECT_EQ(Img.DynInfo.MinOSVersion, Escaped("minimum-os"));
  EXPECT_EQ(Img.ExceptionMetadata.Diagnostics[0], Escaped("image-diagnostic"));
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].PersonalityName,
            Escaped("personality"));
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].Diagnostics[0],
            Escaped("function-diagnostic"));
}

TEST(RuntimeMetadata, RejectsInvalidStubIndexWithoutChangingImports) {
  BinaryImage Img;
  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = 0x4000;
  Img.Imports.push_back(Imp);

  EXPECT_FALSE(Img.recordImportStub(0x1000, 1));
  EXPECT_TRUE(Img.ImportStubIndices.empty());
  EXPECT_EQ(Img.Imports[0].IATAddr, 0x4000u);
}

TEST(RuntimeMetadata, KeepsStubIndicesStableAsImportStorageGrows) {
  BinaryImage Img;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));

  Import First;
  First.Name = "duplicate_name";
  First.IATAddr = 0x3000;
  Img.Imports.push_back(First);
  Import Second = First;
  Second.IATAddr = 0x3008;
  Img.Imports.push_back(Second);
  ASSERT_TRUE(Img.recordImportStub(0x1010, 0));
  ASSERT_TRUE(Img.recordImportStub(0x1020, 1));
  EXPECT_FALSE(Img.recordImportStub(0x1010, 1));

  for (size_t I = 0; I < 128; ++I)
    Img.Imports.push_back(Import{});

  EXPECT_EQ(Img.findImportAt(0x1010), &Img.Imports[0]);
  EXPECT_EQ(Img.findImportAt(0x1020), &Img.Imports[1]);
  EXPECT_EQ(Img.Imports[0].IATAddr, 0x3000u);
  EXPECT_EQ(Img.Imports[1].IATAddr, 0x3008u);
}

TEST(RuntimeMetadata, RecognizesCheckedStubRanges) {
  BinaryImage Img;
  Img.Segments.push_back(makeSegment(0x2000, 0x40, true));
  ASSERT_TRUE(Img.recordImportStubRange(0x2000, 0x20));
  EXPECT_TRUE(Img.isImportStubAt(0x2000));
  EXPECT_TRUE(Img.isImportStubAt(0x201f));
  EXPECT_FALSE(Img.isImportStubAt(0x2020));
  EXPECT_FALSE(Img.recordImportStubRange(InvalidVA - 1, 2));
  EXPECT_FALSE(Img.recordImportStubRange(0x2030, 0x20));
}

TEST(RuntimeMetadata, RecordsOnlyMappedRuntimeFunctions) {
  BinaryImage Img;
  Img.Segments.push_back(makeSegment(0, 0x40, true));
  Img.Segments.push_back(makeSegment(0x1000, 0x40, false));

  EXPECT_TRUE(Img.recordRuntimeFunction(0));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0));
  EXPECT_FALSE(Img.recordRuntimeFunction(0x1000));
  EXPECT_FALSE(Img.recordRuntimeFunction(0x2000));
}

TEST(RuntimeMetadata, RuntimeFunctionsRespectExactSectionCodeOwnership) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));

    BinaryImage Img;
    Img.Format = Format;
    Img.Segments.push_back(makeSegment(0x1000, 0x100, true));

    Section Code;
    Code.Name = "code";
    Code.VA = 0x1010;
    Code.Size = 0x10;
    Code.FileSz = Code.Size;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    if (Format == BinaryFormat::MachO)
      Code.Type = static_cast<uint32_t>(llvm::MachO::S_REGULAR) |
                  static_cast<uint32_t>(llvm::MachO::S_ATTR_PURE_INSTRUCTIONS);
    Img.Sections.push_back(std::move(Code));

    Section Data;
    Data.Name = "data";
    Data.VA = 0x1040;
    Data.Size = 0x10;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    if (Format == BinaryFormat::MachO) {
      Data.Flags = Data.Flags | SegmentFlags::Executable;
      Data.Type = llvm::MachO::S_REGULAR;
    }
    Img.Sections.push_back(std::move(Data));

    EXPECT_TRUE(Img.recordRuntimeFunction(0x1010));
    EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1010));
    EXPECT_FALSE(Img.recordRuntimeFunction(0x1040));
    EXPECT_FALSE(Img.isRuntimeFunctionAt(0x1040));
    EXPECT_FALSE(Img.recordRuntimeFunction(0x1080));
    EXPECT_FALSE(Img.isRuntimeFunctionAt(0x1080));

    BinaryImage Sectionless = Img;
    Sectionless.Sections.clear();
    EXPECT_TRUE(Sectionless.recordRuntimeFunction(0x1080));
    EXPECT_TRUE(Sectionless.isRuntimeFunctionAt(0x1080));
  }
}

TEST(RuntimeMetadata, ChoosesExportSymbolThenAutomaticFunctionName) {
  BinaryImage Img;
  Symbol Sym = Symbol::makeFunc(0x1000);
  Sym.Name = "symbol_name";
  Img.Symbols.push_back(Sym);

  Export Exp;
  Exp.Name = "export_name";
  Exp.Addr = 0x1000;
  Img.Exports.push_back(Exp);

  EXPECT_EQ(Img.getFunctionNameAt(0x1000), "export_name");
  EXPECT_EQ(Img.getFunctionNameAt(0x2000), "sub_2000");
}

TEST(RuntimeMetadata, ParsesRVADelayImportWithExactIATSlot) {
  DelayImportImage Fixture =
      makeDelayImportImage(/*Is64=*/false, /*LegacyVA=*/false,
                           /*ByOrdinal=*/false);

  EXPECT_EQ(coff_loader::parseDelayImportDescriptor(Fixture.Desc, Fixture.Img),
            1u);
  ASSERT_EQ(Fixture.Img.Imports.size(), 1u);
  EXPECT_EQ(Fixture.Img.Imports[0].Module, "example.dll [delay]");
  EXPECT_EQ(Fixture.Img.Imports[0].Name, "delayed_function");
  EXPECT_EQ(Fixture.Img.Imports[0].IATAddr, 0x401100u);
  ASSERT_EQ(Fixture.Img.ImportStorageSlots.count(0x401100u), 1u);
  EXPECT_EQ(Fixture.Img.ImportStorageSlots.at(0x401100u).Name,
            "delayed_function");
}

TEST(RuntimeMetadata, ParsesLegacyVADelayImportAnd64BitOrdinal) {
  DelayImportImage Fixture =
      makeDelayImportImage(/*Is64=*/true, /*LegacyVA=*/true,
                           /*ByOrdinal=*/true);

  EXPECT_EQ(coff_loader::parseDelayImportDescriptor(Fixture.Desc, Fixture.Img),
            1u);
  ASSERT_EQ(Fixture.Img.Imports.size(), 1u);
  EXPECT_EQ(Fixture.Img.Imports[0].Name, "ord_37");
  EXPECT_EQ(Fixture.Img.Imports[0].Ordinal, 37u);
  EXPECT_EQ(Fixture.Img.Imports[0].IATAddr, 0x401100u);
  ASSERT_EQ(Fixture.Img.ImportStorageSlots.count(0x401100u), 1u);
  EXPECT_EQ(Fixture.Img.ImportStorageSlots.at(0x401100u).Name, "ord_37");
}

TEST(RuntimeMetadata, Uses64BitStrideForEachDelayIATSlot) {
  DelayImportImage Fixture =
      makeDelayImportImage(/*Is64=*/true, /*LegacyVA=*/false,
                           /*ByOrdinal=*/true);
  Segment &Seg = Fixture.Img.Segments.back();
  constexpr size_t IATOffset = 0x100;
  constexpr size_t INTOffset = 0x200;
  writePtr(Seg.Data.data() + INTOffset + 8, (uint64_t(1) << 63) | 38, true);
  writePtr(Seg.Data.data() + IATOffset + 8, 0xfeedbeef, true);
  writePtr(Seg.Data.data() + INTOffset + 16, 0, true);
  writePtr(Seg.Data.data() + IATOffset + 16, 0, true);

  EXPECT_EQ(coff_loader::parseDelayImportDescriptor(Fixture.Desc, Fixture.Img),
            2u);
  ASSERT_EQ(Fixture.Img.Imports.size(), 2u);
  EXPECT_EQ(Fixture.Img.Imports[0].IATAddr, 0x401100u);
  EXPECT_EQ(Fixture.Img.Imports[1].IATAddr, 0x401108u);
  EXPECT_EQ(Fixture.Img.Imports[1].Ordinal, 38u);
  EXPECT_EQ(Fixture.Img.ImportStorageSlots.size(), 2u);
  EXPECT_EQ(Fixture.Img.ImportStorageSlots.at(0x401100u).Name, "ord_37");
  EXPECT_EQ(Fixture.Img.ImportStorageSlots.at(0x401108u).Name, "ord_38");
}

TEST(RuntimeMetadata, RejectsMalformedDelayImportDescriptor) {
  DelayImportImage Fixture =
      makeDelayImportImage(/*Is64=*/false, /*LegacyVA=*/false,
                           /*ByOrdinal=*/false);
  Fixture.Desc.DelayImportNameTable = 0xfffffff0u;

  EXPECT_EQ(coff_loader::parseDelayImportDescriptor(Fixture.Desc, Fixture.Img),
            0u);
  EXPECT_TRUE(Fixture.Img.Imports.empty());
  EXPECT_TRUE(Fixture.Img.ImportStorageSlots.empty());
}

TEST(RuntimeMetadata, CollectsELFLifecycleSectionsWithoutDuplicates) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));

  Segment &Arrays = Img.Segments.back();
  writePtr(Arrays.Data.data(), 0x1010, true);
  writePtr(Arrays.Data.data() + 8, 0x1010, true);
  writePtr(Arrays.Data.data() + 0x20, 0x1020, true);
  writePtr(Arrays.Data.data() + 0x40, 0x1030, true);
  writePtr(Arrays.Data.data() + 0x60, InvalidVA, true);

  Section Preinit;
  Preinit.Name = section_names::elf::PreinitArray;
  Preinit.VA = 0x3000;
  Preinit.Size = 16;
  Preinit.Type = llvm::ELF::SHT_PREINIT_ARRAY;
  Img.Sections.push_back(Preinit);

  Section Ctors;
  Ctors.Name = section_names::elf::Ctors;
  Ctors.VA = 0x3020;
  Ctors.Size = 8;
  Img.Sections.push_back(Ctors);

  Section Fini;
  Fini.Name = section_names::elf::FiniArray;
  Fini.VA = 0x3040;
  Fini.Size = 8;
  Fini.Type = llvm::ELF::SHT_FINI_ARRAY;
  Img.Sections.push_back(Fini);

  Section Dtors;
  Dtors.Name = section_names::elf::Dtors;
  Dtors.VA = 0x3060;
  Dtors.Size = 8;
  Img.Sections.push_back(Dtors);

  elf_loader::parseRuntimeSections(Img);

  EXPECT_EQ(Img.DynInfo.PreinitArray, (std::vector<va_t>{0x1010}));
  EXPECT_EQ(Img.DynInfo.InitArray, (std::vector<va_t>{0x1020}));
  EXPECT_EQ(Img.DynInfo.FiniArray, (std::vector<va_t>{0x1030}));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1010));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1020));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1030));
}

TEST(RuntimeMetadata, RecordsGNUELFIRelativeResolversForEverySupportedArch) {
  struct Case {
    Arch TargetArch;
    Bitness Bits;
    uint32_t Type;
    va_t EncodedResolver;
  };
  const Case Cases[] = {
      {Arch::X86, Bitness::Bits32, llvm::ELF::R_386_IRELATIVE, 0x1010},
      {Arch::X64, Bitness::Bits64, llvm::ELF::R_X86_64_IRELATIVE, 0x1010},
      {Arch::ARM, Bitness::Bits32, llvm::ELF::R_ARM_IRELATIVE, 0x1011},
      {Arch::AArch64, Bitness::Bits64, llvm::ELF::R_AARCH64_IRELATIVE, 0x1010},
  };

  for (const Case &C : Cases) {
    BinaryImage Img;
    Img.Arch = C.TargetArch;
    Img.Bits = C.Bits;
    Img.Segments.push_back(makeSegment(0x1000, 0x100, true));

    EXPECT_TRUE(elf_loader::recordIRelativeResolver(
        C.Type, 0, static_cast<int64_t>(C.EncodedResolver), Img));
    EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1010));
  }

  BinaryImage Invalid;
  Invalid.Arch = Arch::X64;
  Invalid.Bits = Bitness::Bits64;
  Invalid.Segments.push_back(makeSegment(0x1000, 0x100, true));
  EXPECT_FALSE(elf_loader::recordIRelativeResolver(
      llvm::ELF::R_X86_64_IRELATIVE, 0, int64_t(-1), Invalid));
  EXPECT_FALSE(elf_loader::recordIRelativeResolver(
      llvm::ELF::R_X86_64_IRELATIVE, 0, int64_t(0x2000), Invalid));
  EXPECT_FALSE(elf_loader::recordIRelativeResolver(
      llvm::ELF::R_386_IRELATIVE, 0, int64_t(0x1010), Invalid));
}

TEST(RuntimeMetadata, ReadsImplicitELFIRelativeResolverFromRelocationSlot) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  Img.Segments.push_back(makeSegment(0x3000, 0x20, false));
  writePtr(Img.Segments.back().Data.data(), 0x1020, false);

  EXPECT_TRUE(elf_loader::recordIRelativeResolver(llvm::ELF::R_386_IRELATIVE,
                                                  0x3000, std::nullopt, Img));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1020));
}

TEST(RuntimeMetadata, CollectsEveryMachORuntimeFunctionSectionKind) {
  BinaryImage Img;
  Img.Format = BinaryFormat::MachO;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  Img.Segments.push_back(makeSegment(0x3000, 0x100, false));

  Segment &Arrays = Img.Segments.back();
  writePtr(Arrays.Data.data(), 0x1010, true);
  writePtr(Arrays.Data.data() + 0x20, 0x1020, true);
  writePtr(Arrays.Data.data() + 0x40, 0x1030, true);
  writeLE<uint32_t>(Arrays.Data.data() + 0x60, 0x40);
  writeLE<uint32_t>(Arrays.Data.data() + 0x64, 0xfffffff0u);

  std::vector<macho_loader::SectionInfo> Sections;
  Sections.push_back(
      {"", "", 0x3000, 8, 0, llvm::MachO::S_MOD_INIT_FUNC_POINTERS, 0});
  Sections.push_back(
      {"", "", 0x3020, 8, 0, llvm::MachO::S_MOD_TERM_FUNC_POINTERS, 0});
  Sections.push_back({"", "", 0x3040, 8, 0,
                      llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS, 0});
  Sections.push_back(
      {"", "", 0x3060, 8, 0, llvm::MachO::S_INIT_FUNC_OFFSETS, 0});

  macho_loader::parseRuntimeFunctionSections(Sections, 0x1000, Img);

  EXPECT_EQ(Img.DynInfo.InitArray, (std::vector<va_t>{0x1010, 0x1030, 0x1040}));
  EXPECT_EQ(Img.DynInfo.FiniArray, (std::vector<va_t>{0x1020}));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1010));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1020));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1030));
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1040));
}

TEST(RuntimeMetadata, KeepsMachOLCMainPatchableAheadOfThreadState) {
  llvm::MachO::entry_point_command Main{};
  Main.cmd = llvm::MachO::LC_MAIN;
  Main.cmdsize = static_cast<uint32_t>(sizeof(Main));
  Main.entryoff = 0x30;
  std::vector<uint8_t> Bytes =
      makeMachOImage({makeThreadCommand(llvm::MachO::LC_UNIXTHREAD, 0x1010),
                      objectBytes(Main)});
  auto Obj = createMachOObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  macho_loader::parseEntryPoint(*Obj, Img, 0x1000);

  EXPECT_EQ(Img.Entry, 0x1030u);
  EXPECT_FALSE(Img.isRuntimeFunctionAt(0x1030));
  EXPECT_FALSE(Img.isRuntimeFunctionAt(0x1010));
}

TEST(RuntimeMetadata, PrefersMachOUnixThreadAndMarksThreadEntryRuntime) {
  std::vector<uint8_t> Bytes =
      makeMachOImage({makeThreadCommand(llvm::MachO::LC_THREAD, 0x1020),
                      makeThreadCommand(llvm::MachO::LC_UNIXTHREAD, 0x1010)});
  auto Obj = createMachOObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  macho_loader::parseEntryPoint(*Obj, Img, 0x1000);

  EXPECT_EQ(Img.Entry, 0x1010u);
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1010));
  EXPECT_FALSE(Img.isRuntimeFunctionAt(0x1020));
}

TEST(RuntimeMetadata, FallsBackToMachOLCThreadEntry) {
  std::vector<uint8_t> Bytes =
      makeMachOImage({makeThreadCommand(llvm::MachO::LC_THREAD, 0x1020)});
  auto Obj = createMachOObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(makeSegment(0x1000, 0x100, true));
  macho_loader::parseEntryPoint(*Obj, Img, 0x1000);

  EXPECT_EQ(Img.Entry, 0x1020u);
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1020));
}

TEST(RuntimeMetadata, RecordsMachORoutines32And64Commands) {
  llvm::MachO::routines_command Routines32{};
  Routines32.cmd = llvm::MachO::LC_ROUTINES;
  Routines32.cmdsize = static_cast<uint32_t>(sizeof(Routines32));
  Routines32.init_address = 0x1010;
  llvm::MachO::routines_command_64 Routines64{};
  Routines64.cmd = llvm::MachO::LC_ROUTINES_64;
  Routines64.cmdsize = static_cast<uint32_t>(sizeof(Routines64));
  Routines64.init_address = 0x1020;

  // Mach-O permits at most one routines command.  Exercise the 32- and 64-bit
  // variants in separate legal objects rather than constructing a malformed
  // image that LLVM's object reader correctly rejects.
  std::vector<uint8_t> Bytes32 = makeMachOImage({objectBytes(Routines32)});
  auto Obj32 = createMachOObject(Bytes32);
  ASSERT_NE(Obj32, nullptr);
  BinaryImage Img32;
  Img32.Arch = Arch::X64;
  Img32.Bits = Bitness::Bits64;
  Img32.Segments.push_back(makeSegment(0x1000, 0x100, true));
  macho_loader::parseRuntimeLoadCommands(*Obj32, Img32);

  EXPECT_EQ(Img32.DynInfo.InitAddr, 0x1010u);
  EXPECT_TRUE(Img32.isRuntimeFunctionAt(0x1010));

  std::vector<uint8_t> Bytes64 = makeMachOImage({objectBytes(Routines64)});
  auto Obj64 = createMachOObject(Bytes64);
  ASSERT_NE(Obj64, nullptr);
  BinaryImage Img64;
  Img64.Arch = Arch::X64;
  Img64.Bits = Bitness::Bits64;
  Img64.Segments.push_back(makeSegment(0x1000, 0x100, true));
  macho_loader::parseRuntimeLoadCommands(*Obj64, Img64);

  EXPECT_EQ(Img64.DynInfo.InitAddr, 0x1020u);
  EXPECT_TRUE(Img64.isRuntimeFunctionAt(0x1020));
}

} // namespace
