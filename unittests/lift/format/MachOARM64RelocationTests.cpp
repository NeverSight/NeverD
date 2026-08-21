//===- MachOARM64RelocationTests.cpp - Mach-O ARM64 relocations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace neverd;
using namespace llvm::MachO;

constexpr uint64_t SectionVA = 0x100000000ULL;

struct RawRelocation {
  uint32_t Offset = 0;
  uint32_t SymbolNumber = 0;
  bool IsPCRel = false;
  uint32_t Length = 2;
  bool IsExternal = true;
  uint32_t Type = 0;
};

struct SymbolSpec {
  uint64_t Address = 0;
  uint8_t Type = static_cast<uint8_t>(N_SECT | N_EXT);
  uint8_t Section = 1;
};

template <typename T>
void writeObject(std::vector<uint8_t> &Bytes, size_t Offset, const T &Value) {
  ASSERT_LE(Offset + sizeof(T), Bytes.size());
  std::memcpy(Bytes.data() + Offset, &Value, sizeof(T));
}

size_t alignTo(size_t Value, size_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

uint32_t relocationWord(const RawRelocation &Reloc) {
  return (Reloc.SymbolNumber & 0x00ffffffu) | (uint32_t(Reloc.IsPCRel) << 24) |
         ((Reloc.Length & 3u) << 25) | (uint32_t(Reloc.IsExternal) << 27) |
         ((Reloc.Type & 0xfu) << 28);
}

std::vector<uint8_t>
makeObjectWithSymbols(uint32_t CPUType, llvm::ArrayRef<uint8_t> SectionData,
                      llvm::ArrayRef<RawRelocation> Relocations,
                      llvm::ArrayRef<SymbolSpec> Symbols) {
  constexpr size_t HeaderSize = sizeof(mach_header_64);
  constexpr size_t SegmentCommandSize =
      sizeof(segment_command_64) + sizeof(section_64);
  constexpr size_t CommandsSize = SegmentCommandSize + sizeof(symtab_command);
  const size_t DataOffset = alignTo(HeaderSize + CommandsSize, 8);
  const size_t RelocationOffset = alignTo(DataOffset + SectionData.size(), 8);
  const size_t SymbolOffset =
      RelocationOffset + Relocations.size() * sizeof(relocation_info);

  std::string StringTable(1, '\0');
  std::vector<uint32_t> NameOffsets;
  for (size_t I = 0; I < Symbols.size(); ++I) {
    NameOffsets.push_back(static_cast<uint32_t>(StringTable.size()));
    StringTable += "_target" + std::to_string(I);
    StringTable.push_back('\0');
  }
  const size_t StringOffset = SymbolOffset + Symbols.size() * sizeof(nlist_64);
  std::vector<uint8_t> Bytes(StringOffset + StringTable.size());

  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = CPUType;
  Header.cpusubtype = CPUType == CPU_TYPE_ARM64 ? CPU_SUBTYPE_ARM64_ALL
                                                : CPU_SUBTYPE_X86_64_ALL;
  Header.filetype = MH_OBJECT;
  Header.ncmds = 2;
  Header.sizeofcmds = CommandsSize;
  writeObject(Bytes, 0, Header);

  segment_command_64 Segment{};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = SegmentCommandSize;
  Segment.vmaddr = SectionVA;
  Segment.vmsize = SectionData.size();
  Segment.fileoff = DataOffset;
  Segment.filesize = SectionData.size();
  Segment.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment.initprot = Segment.maxprot;
  Segment.nsects = 1;
  writeObject(Bytes, HeaderSize, Segment);

  section_64 Section{};
  std::memcpy(Section.sectname, "__text", 6);
  std::memcpy(Section.segname, "__TEXT", 6);
  Section.addr = SectionVA;
  Section.size = SectionData.size();
  Section.offset = DataOffset;
  Section.align = 2;
  Section.reloff = RelocationOffset;
  Section.nreloc = Relocations.size();
  Section.flags =
      S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
  writeObject(Bytes, HeaderSize + sizeof(segment_command_64), Section);

  symtab_command Symtab{};
  Symtab.cmd = LC_SYMTAB;
  Symtab.cmdsize = sizeof(symtab_command);
  Symtab.symoff = SymbolOffset;
  Symtab.nsyms = Symbols.size();
  Symtab.stroff = StringOffset;
  Symtab.strsize = StringTable.size();
  writeObject(Bytes, HeaderSize + SegmentCommandSize, Symtab);

  std::copy(SectionData.begin(), SectionData.end(), Bytes.begin() + DataOffset);
  for (size_t I = 0; I < Relocations.size(); ++I) {
    const size_t Offset = RelocationOffset + I * sizeof(relocation_info);
    neverd::writeLE<uint32_t>(Bytes.data() + Offset, Relocations[I].Offset);
    neverd::writeLE<uint32_t>(Bytes.data() + Offset + 4,
                              relocationWord(Relocations[I]));
  }
  for (size_t I = 0; I < Symbols.size(); ++I) {
    nlist_64 Symbol{};
    Symbol.n_strx = NameOffsets[I];
    Symbol.n_type = Symbols[I].Type;
    Symbol.n_sect = Symbols[I].Section;
    Symbol.n_value = Symbols[I].Address;
    writeObject(Bytes, SymbolOffset + I * sizeof(nlist_64), Symbol);
  }
  std::copy(StringTable.begin(), StringTable.end(),
            Bytes.begin() + StringOffset);
  return Bytes;
}

std::vector<uint8_t> makeObject(uint32_t CPUType,
                                llvm::ArrayRef<uint8_t> SectionData,
                                llvm::ArrayRef<RawRelocation> Relocations,
                                llvm::ArrayRef<uint64_t> SymbolAddresses) {
  std::vector<SymbolSpec> Symbols;
  Symbols.reserve(SymbolAddresses.size());
  for (uint64_t Address : SymbolAddresses)
    Symbols.push_back(SymbolSpec{Address});
  return makeObjectWithSymbols(CPUType, SectionData, Relocations, Symbols);
}

std::unique_ptr<llvm::object::MachOObjectFile>
parseObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Data, "MachO relocation test"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

BinaryImage makeImage(Arch TargetArch, llvm::ArrayRef<uint8_t> Data) {
  BinaryImage Image;
  Image.Arch = TargetArch;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;

  Segment Seg;
  Seg.Name = "__TEXT";
  Seg.VA = SectionVA;
  Seg.Size = Data.size();
  Seg.FileSz = Seg.Size;
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Data.begin(), Data.end());
  Image.Segments.push_back(std::move(Seg));

  Section Sec;
  Sec.Name = "__text";
  Sec.SegmentName = "__TEXT";
  Sec.VA = SectionVA;
  Sec.Size = Data.size();
  Sec.FileSz = Sec.Size;
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Sec.Data.assign(Data.begin(), Data.end());
  Image.Sections.push_back(std::move(Sec));
  return Image;
}

std::vector<uint8_t>
applyRelocations(Arch TargetArch, uint32_t CPUType,
                 llvm::ArrayRef<uint8_t> SectionData,
                 llvm::ArrayRef<RawRelocation> Relocations,
                 llvm::ArrayRef<uint64_t> SymbolAddresses) {
  auto Bytes = makeObject(CPUType, SectionData, Relocations, SymbolAddresses);
  auto Obj = parseObject(Bytes);
  if (!Obj)
    return {};
  BinaryImage Image = makeImage(TargetArch, SectionData);
  if (llvm::Error Err =
          neverd::macho_loader::applyObjectRelocations(*Obj, Image)) {
    ADD_FAILURE() << llvm::toString(std::move(Err));
    return {};
  }
  return Image.Segments.front().Data;
}

bool relocationApplicationFails(Arch TargetArch, uint32_t CPUType,
                                llvm::ArrayRef<uint8_t> SectionData,
                                llvm::ArrayRef<RawRelocation> Relocations,
                                llvm::ArrayRef<uint64_t> SymbolAddresses) {
  auto Bytes = makeObject(CPUType, SectionData, Relocations, SymbolAddresses);
  auto Obj = parseObject(Bytes);
  if (!Obj)
    return true;
  BinaryImage Image = makeImage(TargetArch, SectionData);
  llvm::Error Err = neverd::macho_loader::applyObjectRelocations(*Obj, Image);
  if (!Err)
    return false;
  llvm::consumeError(std::move(Err));
  return true;
}

std::vector<uint8_t>
applyRelocationsWithSymbols(Arch TargetArch, uint32_t CPUType,
                            llvm::ArrayRef<uint8_t> SectionData,
                            llvm::ArrayRef<RawRelocation> Relocations,
                            llvm::ArrayRef<SymbolSpec> Symbols) {
  auto Bytes =
      makeObjectWithSymbols(CPUType, SectionData, Relocations, Symbols);
  auto Obj = parseObject(Bytes);
  if (!Obj)
    return {};
  BinaryImage Image = makeImage(TargetArch, SectionData);
  if (llvm::Error Err =
          neverd::macho_loader::applyObjectRelocations(*Obj, Image)) {
    ADD_FAILURE() << llvm::toString(std::move(Err));
    return {};
  }
  return Image.Segments.front().Data;
}

uint32_t readInstruction(llvm::ArrayRef<uint8_t> Data, size_t Offset = 0) {
  EXPECT_LE(Offset + sizeof(uint32_t), Data.size());
  return neverd::readLE<uint32_t>(Data.data() + Offset);
}

void writeInstruction(std::vector<uint8_t> &Data, size_t Offset,
                      uint32_t Instruction) {
  ASSERT_LE(Offset + sizeof(uint32_t), Data.size());
  neverd::writeLE<uint32_t>(Data.data() + Offset, Instruction);
}

RawRelocation addendRelocation(uint32_t Offset, int32_t Addend) {
  return {Offset, static_cast<uint32_t>(Addend) & 0x00ffffffu,
          false,  2,
          false,  ARM64_RELOC_ADDEND};
}

RawRelocation arm64InstructionRelocation(uint32_t Offset, uint32_t Type) {
  const bool IsPCRel =
      Type == ARM64_RELOC_BRANCH26 || Type == ARM64_RELOC_PAGE21;
  return {Offset, 0, IsPCRel, 2, true, Type};
}

TEST(MachOARM64Relocation, AddendPairsWithBranch26) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x40, 0x94000000u);
  const uint64_t Target = SectionVA + 0x180;
  const std::vector<RawRelocation> Relocations = {
      addendRelocation(0x40, 12),
      arm64InstructionRelocation(0x40, ARM64_RELOC_BRANCH26)};

  auto Patched = applyRelocations(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                  Relocations, {Target});
  ASSERT_FALSE(Patched.empty());
  const uint32_t Encoded =
      static_cast<uint32_t>((Target + 12 - (SectionVA + 0x40)) >> 2) &
      0x03ffffffu;
  EXPECT_EQ(readInstruction(Patched, 0x40), 0x94000000u | Encoded);
}

TEST(MachOARM64Relocation, AddendPairsWithPage21) {
  std::vector<uint8_t> Data(0x6000);
  writeInstruction(Data, 0x100, 0x90000008u);
  const uint64_t Target = SectionVA + 0x2100;
  const std::vector<RawRelocation> Relocations = {
      addendRelocation(0x100, 0x1000),
      arm64InstructionRelocation(0x100, ARM64_RELOC_PAGE21)};

  auto Patched = applyRelocations(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                  Relocations, {Target});
  ASSERT_FALSE(Patched.empty());
  const uint64_t PageDelta =
      ((Target + 0x1000) & ~0xfffULL) - ((SectionVA + 0x100) & ~0xfffULL);
  const uint32_t Expected = 0x90000008u | (((PageDelta >> 12) & 3u) << 29) |
                            (((PageDelta >> 14) & 0x7ffffu) << 5);
  EXPECT_EQ(readInstruction(Patched, 0x100), Expected);
}

TEST(MachOARM64Relocation, AddendPairsWithPageOff12) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x80, 0x91000021u);
  const uint64_t Target = SectionVA + 0x120;
  const std::vector<RawRelocation> Relocations = {
      addendRelocation(0x80, 0x28),
      arm64InstructionRelocation(0x80, ARM64_RELOC_PAGEOFF12)};

  auto Patched = applyRelocations(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                  Relocations, {Target});
  ASSERT_FALSE(Patched.empty());
  EXPECT_EQ(readInstruction(Patched, 0x80), 0x91000021u | (0x148u << 10));
}

struct PageOffCase {
  uint32_t Instruction;
};

class MachOARM64PageOffRelocation
    : public ::testing::TestWithParam<PageOffCase> {};

TEST_P(MachOARM64PageOffRelocation, RejectsNonzeroEmbeddedImmediate) {
  const PageOffCase Case = GetParam();
  std::vector<uint8_t> Data(0x3000);
  const uint32_t EmbeddedImmediate = 3;
  writeInstruction(Data, 0x20, Case.Instruction | (EmbeddedImmediate << 10));
  const uint64_t Target = SectionVA + 0x120;
  const std::vector<RawRelocation> Relocations = {
      arm64InstructionRelocation(0x20, ARM64_RELOC_PAGEOFF12)};

  EXPECT_TRUE(relocationApplicationFails(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                         Relocations, {Target}));
}

INSTANTIATE_TEST_SUITE_P(AccessSizes, MachOARM64PageOffRelocation,
                         ::testing::Values(PageOffCase{0x39400000u},
                                           PageOffCase{0x79400000u},
                                           PageOffCase{0xb9400000u},
                                           PageOffCase{0xf9400000u},
                                           PageOffCase{0x3dc00000u}));

TEST(MachOARM64Relocation, OrphanAddendDoesNotModifyInstruction) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x20, 0x94000009u);
  const std::vector<RawRelocation> Relocations = {addendRelocation(0x20, 7)};

  EXPECT_TRUE(relocationApplicationFails(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                         Relocations, {}));
}

TEST(MachOARM64Relocation, DuplicateAddendsPoisonTheirTarget) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x20, 0x90000000u);
  const std::vector<RawRelocation> Relocations = {
      addendRelocation(0x20, 4), addendRelocation(0x20, 8),
      arm64InstructionRelocation(0x20, ARM64_RELOC_PAGE21)};

  EXPECT_TRUE(relocationApplicationFails(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                         Relocations, {SectionVA + 0x2000}));
}

TEST(MachOARM64Relocation, AddendAtDifferentOffsetPoisonsItsTarget) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x24, 0x14000000u);
  const std::vector<RawRelocation> Relocations = {
      addendRelocation(0x20, 4),
      arm64InstructionRelocation(0x24, ARM64_RELOC_BRANCH26)};

  EXPECT_TRUE(relocationApplicationFails(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                         Relocations, {SectionVA + 0x100}));
}

TEST(MachOARM64Relocation, ExplicitAddendRejectsNonzeroPageOffImmediate) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x20, 0xf9400000u | (3u << 10));
  const std::vector<RawRelocation> Relocations = {
      addendRelocation(0x20, 8),
      arm64InstructionRelocation(0x20, ARM64_RELOC_PAGEOFF12)};
  EXPECT_TRUE(relocationApplicationFails(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                         Relocations, {SectionVA + 0x120}));
}

TEST(MachOARM64Relocation, InvalidInstructionMetadataFailsClosed) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x20, 0x90000000u);
  RawRelocation Reloc = arm64InstructionRelocation(0x20, ARM64_RELOC_PAGE21);
  Reloc.IsPCRel = false;
  EXPECT_TRUE(relocationApplicationFails(Arch::AArch64, CPU_TYPE_ARM64, Data,
                                         {Reloc}, {SectionVA + 0x1000}));
}

TEST(MachOARM64Relocation, UndefinedExternalDoesNotPatchInstruction) {
  std::vector<uint8_t> Data(0x3000);
  writeInstruction(Data, 0x20, 0x90000008u);
  const std::vector<SymbolSpec> Symbols = {
      SymbolSpec{0, static_cast<uint8_t>(N_UNDF | N_EXT), 0}};
  auto Patched = applyRelocationsWithSymbols(
      Arch::AArch64, CPU_TYPE_ARM64, Data,
      {arm64InstructionRelocation(0x20, ARM64_RELOC_PAGE21)}, Symbols);
  ASSERT_FALSE(Patched.empty());
  EXPECT_EQ(readInstruction(Patched, 0x20), 0x90000008u);
}

TEST(MachORelocation, LocalUnsignedCompletesSubtractorPair) {
  for (auto [TargetArch, CPUType, SubtractorType, UnsignedType] :
       {std::tuple{Arch::AArch64, uint32_t(CPU_TYPE_ARM64),
                   uint32_t(ARM64_RELOC_SUBTRACTOR),
                   uint32_t(ARM64_RELOC_UNSIGNED)},
        std::tuple{Arch::X64, uint32_t(CPU_TYPE_X86_64),
                   uint32_t(X86_64_RELOC_SUBTRACTOR),
                   uint32_t(X86_64_RELOC_UNSIGNED)}}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    std::vector<uint8_t> Data(0x3000);
    constexpr uint32_t Offset = 0x80;
    constexpr uint64_t Subtrahend = SectionVA + 0x200;
    const uint64_t Minuend = SectionVA + Offset + 2;
    writeObject(Data, Offset, Minuend);
    const std::vector<RawRelocation> Relocations = {
        {Offset, 0, false, 3, true, SubtractorType},
        {Offset, 1, false, 3, false, UnsignedType}};
    const std::vector<SymbolSpec> Symbols = {{Subtrahend}};

    auto Patched = applyRelocationsWithSymbols(TargetArch, CPUType, Data,
                                               Relocations, Symbols);
    ASSERT_FALSE(Patched.empty());
    const int64_t Expected =
        static_cast<int64_t>(Minuend) - static_cast<int64_t>(Subtrahend);
    EXPECT_EQ(neverd::readLE<uint64_t>(Patched.data() + Offset),
              static_cast<uint64_t>(Expected));
  }
}

TEST(MachOX64Relocation, PCRelativeUsesEndOfFieldAddendAndTailSize) {
  struct Case {
    uint32_t Type;
    uint64_t Tail;
  };
  for (Case C : {Case{X86_64_RELOC_SIGNED, 0}, Case{X86_64_RELOC_BRANCH, 0},
                 Case{X86_64_RELOC_SIGNED_1, 1}, Case{X86_64_RELOC_SIGNED_2, 2},
                 Case{X86_64_RELOC_SIGNED_4, 4}}) {
    SCOPED_TRACE(C.Type);
    std::vector<uint8_t> Data(0x3000);
    constexpr uint32_t Offset = 0x40;
    constexpr int32_t Addend = 8;
    writeObject(Data, Offset, Addend);
    constexpr uint64_t Target = SectionVA + 0x180;
    const RawRelocation Reloc{Offset, 0, true, 2, true, C.Type};

    auto Patched =
        applyRelocations(Arch::X64, CPU_TYPE_X86_64, Data, {Reloc}, {Target});
    ASSERT_FALSE(Patched.empty());
    const int64_t Expected =
        static_cast<int64_t>(Target + Addend) -
        static_cast<int64_t>(SectionVA + Offset + 4 + C.Tail);
    EXPECT_EQ(neverd::readLE<int32_t>(Patched.data() + Offset), Expected);
  }
}

} // namespace
