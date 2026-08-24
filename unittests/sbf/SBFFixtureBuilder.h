//===- SBFFixtureBuilder.h - Deterministic Solana SBF ELF fixtures -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SBF_SBFFIXTUREBUILDER_H
#define NEVERD_UNITTESTS_SBF_SBFFIXTUREBUILDER_H

#include "neverd/loader/BinaryImageRelocation.h"
#include "neverd/object/SectionNames.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/image/SBFVersion.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/MathExtras.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::sbf::test {

inline constexpr llvm::StringLiteral
    kDefaultStrictDebugSymbolName("named_entry");

struct StrictELFFunctionSymbol {
  std::string Name;
  size_t EntrySlot = 0;
  size_t SlotCount = 0;
};

struct LegacyELFOptions {
  Version TheVersion = Version::V0;
  uint16_t Machine = kELFMachineBPF;
  bool AddWritableData = false;
  bool AddReadOnlyData = false;
  bool TextIsAllocatable = true;
  bool TextIsNoBits = false;
  bool ReadOnlyDataIsAllocatable = true;
  bool ReadOnlyDataIsExecutable = false;
  bool DataIsNoBits = false;
  uint64_t VirtualAddressBase = 0;
  std::optional<uint64_t> DataVirtualAddress;
  std::string DataSectionName = section_names::elf::Data;
  std::vector<uint8_t> Text;
};

struct StrictELFOptions {
  Version TheVersion = Version::V3;
  uint16_t Machine = kELFMachineBPF;
  bool AddRodata = false;
  bool AddDebugSymbols = false;
  size_t EntrySlot = 0;
  std::string DebugSymbolName = kDefaultStrictDebugSymbolName.str();
  std::vector<StrictELFFunctionSymbol> FunctionSymbols;
  std::vector<uint8_t> Text;
};

enum class LegacyDynamicTableSource : uint8_t {
  ProgramHeader,
  SectionHeader,
  InvalidProgramHeaderWithSectionFallback,
};

enum class LegacyDuplicateMetadataSection : uint8_t {
  None,
  Symtab,
  Strtab,
  Dynstr,
};

inline constexpr llvm::StringLiteral
    kDefaultDynamicSymbolName("dynamic_target");
inline constexpr uint32_t kLegacyFixtureDynamicSymbolIndex = 1;

struct LegacyDynamicRelocationSpec {
  size_t TextByteOffset = 0;
  Relocation Type = Relocation::Call32;
  uint32_t SymbolIndex = kLegacyFixtureDynamicSymbolIndex;
  std::optional<uint64_t> RawOffset;
};

struct LegacyDynamicELFOptions {
  LegacyDynamicTableSource DynamicSource =
      LegacyDynamicTableSource::ProgramHeader;
  std::vector<uint8_t> Text;
  std::vector<LegacyDynamicRelocationSpec> Relocations;
  std::string DynamicSymbolName = kDefaultDynamicSymbolName.str();
  std::string StaticSymbolName;
  uint64_t DynamicSymbolValue = 0;
  uint64_t DynamicSymbolSize = 0;
  uint8_t DynamicSymbolBinding = llvm::ELF::STB_GLOBAL;
  uint8_t DynamicSymbolType = llvm::ELF::STT_NOTYPE;
  uint16_t DynamicSymbolSectionIndex = llvm::ELF::SHN_UNDEF;
  bool AddSameNamedStaticFunction = false;
  bool MalformStaticSymbolTableSize = false;
  bool AddMalformedUnusedRel = false;
  bool AddIgnoredDynamicRela = false;
  bool EmitDynamicRel = true;
  bool MisalignDynamicRel = false;
  LegacyDuplicateMetadataSection DuplicateMetadataSection =
      LegacyDuplicateMetadataSection::None;
};

inline std::vector<uint8_t>
buildLegacyELF(const LegacyELFOptions &Options = {}) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Shdr = ELFT::Shdr;
  using namespace llvm::ELF;

  std::string SectionNames(1, '\0');
  auto AppendSectionName = [&](llvm::StringRef Name) {
    const uint32_t Offset = static_cast<uint32_t>(SectionNames.size());
    SectionNames.append(Name.data(), Name.size());
    SectionNames.push_back('\0');
    return Offset;
  };
  const uint32_t TextName = AppendSectionName(kTextSectionName);
  const uint32_t DataName = AppendSectionName(Options.DataSectionName);
  const uint32_t StringTableName =
      AppendSectionName(section_names::elf::Shstrtab);
  constexpr uint64_t SectionAlignment = kInstructionSize;
  constexpr size_t NullSectionIndex = 0;
  constexpr size_t TextSectionIndex = 1;
  constexpr size_t DataSectionIndex = 2;
  constexpr size_t StringTableSectionIndex = 3;
  constexpr size_t SectionCount = 4;

  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  if (!Exit)
    return {};
  std::array<uint8_t, kInstructionSize> DefaultText{};
  DefaultText[kOpcodeOffset] = Exit->Encoding;
  const llvm::ArrayRef<uint8_t> Text =
      Options.Text.empty() ? llvm::ArrayRef<uint8_t>(DefaultText)
                           : llvm::ArrayRef<uint8_t>(Options.Text);
  std::array<uint8_t, kInstructionSize> Data{};
  const bool HasData = Options.AddWritableData || Options.AddReadOnlyData;

  const uint64_t TextOffset = llvm::alignTo(sizeof(Elf_Ehdr), SectionAlignment);
  const uint64_t DataOffset = TextOffset + Text.size();
  const uint64_t StringTableOffset = DataOffset + (HasData ? Data.size() : 0);
  const uint64_t SectionTableOffset = llvm::alignTo(
      StringTableOffset + SectionNames.size(), kELF64RecordAlignment);
  const uint64_t FileSize =
      SectionTableOffset + SectionCount * sizeof(Elf_Shdr);
  std::vector<uint8_t> Bytes(static_cast<size_t>(FileSize), 0);

  Elf_Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_ident[EI_OSABI] = ELFOSABI_NONE;
  Header.e_type = ET_DYN;
  Header.e_machine = Options.Machine;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Options.VirtualAddressBase;
  Header.e_shoff = SectionTableOffset;
  Header.e_flags = static_cast<uint32_t>(Options.TheVersion);
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_phentsize = sizeof(ELFT::Phdr);
  Header.e_shentsize = sizeof(Elf_Shdr);
  Header.e_shnum = SectionCount;
  Header.e_shstrndx = StringTableSectionIndex;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  std::memcpy(Bytes.data() + TextOffset, Text.data(), Text.size());
  if (HasData)
    std::memcpy(Bytes.data() + DataOffset, Data.data(), Data.size());
  std::memcpy(Bytes.data() + StringTableOffset, SectionNames.data(),
              SectionNames.size());

  std::array<Elf_Shdr, SectionCount> Sections{};
  (void)NullSectionIndex;
  Sections[TextSectionIndex].sh_name = TextName;
  Sections[TextSectionIndex].sh_type =
      Options.TextIsNoBits ? SHT_NOBITS : SHT_PROGBITS;
  Sections[TextSectionIndex].sh_flags =
      SHF_EXECINSTR | (Options.TextIsAllocatable ? SHF_ALLOC : 0);
  Sections[TextSectionIndex].sh_addr = Options.VirtualAddressBase;
  Sections[TextSectionIndex].sh_offset = TextOffset;
  Sections[TextSectionIndex].sh_size = Text.size();
  Sections[TextSectionIndex].sh_addralign = SectionAlignment;

  Sections[DataSectionIndex].sh_name = DataName;
  Sections[DataSectionIndex].sh_type =
      Options.DataIsNoBits ? SHT_NOBITS : SHT_PROGBITS;
  Sections[DataSectionIndex].sh_flags =
      Options.AddWritableData
          ? SHF_ALLOC | SHF_WRITE
          : (Options.AddReadOnlyData && Options.ReadOnlyDataIsAllocatable
                 ? SHF_ALLOC
                 : 0);
  if (Options.AddReadOnlyData && Options.ReadOnlyDataIsExecutable)
    Sections[DataSectionIndex].sh_flags |= SHF_EXECINSTR;
  Sections[DataSectionIndex].sh_addr = Options.DataVirtualAddress.value_or(
      Options.VirtualAddressBase + Text.size());
  Sections[DataSectionIndex].sh_offset = DataOffset;
  Sections[DataSectionIndex].sh_size = HasData ? Data.size() : 0;
  Sections[DataSectionIndex].sh_addralign = SectionAlignment;

  Sections[StringTableSectionIndex].sh_name = StringTableName;
  Sections[StringTableSectionIndex].sh_type = SHT_STRTAB;
  Sections[StringTableSectionIndex].sh_offset = StringTableOffset;
  Sections[StringTableSectionIndex].sh_size = SectionNames.size();
  Sections[StringTableSectionIndex].sh_addralign = 1;
  std::memcpy(Bytes.data() + SectionTableOffset, Sections.data(),
              sizeof(Sections));
  return Bytes;
}

/// Build a legacy ELF whose runtime relocations are described exclusively by
/// the dynamic table. The layout deliberately keeps .symtab before .dynsym so
/// same-name tests catch consumers that incorrectly search the global symbol
/// inventory instead of following DT_SYMTAB + r_sym.
inline std::vector<uint8_t>
buildLegacyDynamicELF(const LegacyDynamicELFOptions &Options = {}) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Dyn = ELFT::Dyn;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Phdr = ELFT::Phdr;
  using Elf_Rel = ELFT::Rel;
  using Elf_Rela = ELFT::Rela;
  using Elf_Shdr = ELFT::Shdr;
  using Elf_Sym = ELFT::Sym;
  using namespace llvm::ELF;

  constexpr size_t NullSectionIndex = 0;
  constexpr size_t TextSectionIndex = 1;
  constexpr size_t StringTableSectionIndex = 2;
  constexpr size_t SymbolTableSectionIndex = 3;
  constexpr size_t DynamicStringTableSectionIndex = 4;
  constexpr size_t DynamicSymbolTableSectionIndex = 5;
  constexpr size_t DynamicSectionIndex = 6;
  constexpr size_t RelocationSectionIndex = 7;
  constexpr size_t UnusedRelocationSectionIndex = 8;
  constexpr size_t SectionNameTableSectionIndex = 9;
  constexpr size_t SectionCount = 10;
  constexpr size_t NullSymbolIndex = 0;
  constexpr size_t BoundSymbolIndex = kLegacyFixtureDynamicSymbolIndex;
  constexpr size_t SymbolCount = 2;
  constexpr uint64_t InstructionAlignment = kInstructionSize;

  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  if (!Exit)
    return {};
  std::array<uint8_t, kInstructionSize> DefaultText{};
  DefaultText[kOpcodeOffset] = Exit->Encoding;
  const llvm::ArrayRef<uint8_t> Text =
      Options.Text.empty() ? llvm::ArrayRef<uint8_t>(DefaultText)
                           : llvm::ArrayRef<uint8_t>(Options.Text);

  std::string SectionNames(1, '\0');
  auto AppendSectionName = [&](llvm::StringRef Name) {
    const uint32_t Offset = static_cast<uint32_t>(SectionNames.size());
    SectionNames.append(Name.data(), Name.size());
    SectionNames.push_back('\0');
    return Offset;
  };
  const uint32_t TextName = AppendSectionName(kTextSectionName);
  const uint32_t StringTableName =
      AppendSectionName(section_names::elf::Strtab);
  const uint32_t SymbolTableName =
      AppendSectionName(section_names::elf::Symtab);
  const uint32_t DynamicStringTableName =
      AppendSectionName(section_names::elf::Dynstr);
  const uint32_t DynamicSymbolTableName =
      AppendSectionName(section_names::elf::Dynsym);
  const uint32_t DynamicName = AppendSectionName(section_names::elf::Dynamic);
  const uint32_t SectionNameTableName =
      AppendSectionName(section_names::elf::Shstrtab);

  std::string DynamicNames(1, '\0');
  const uint32_t DynamicSymbolNameOffset =
      static_cast<uint32_t>(DynamicNames.size());
  DynamicNames.append(Options.DynamicSymbolName);
  DynamicNames.push_back('\0');
  const llvm::StringRef StaticSymbolName =
      Options.StaticSymbolName.empty()
          ? llvm::StringRef(Options.DynamicSymbolName)
          : llvm::StringRef(Options.StaticSymbolName);
  std::string StaticNames(1, '\0');
  const uint32_t StaticSymbolNameOffset =
      static_cast<uint32_t>(StaticNames.size());
  StaticNames.append(StaticSymbolName.data(), StaticSymbolName.size());
  StaticNames.push_back('\0');

  const size_t ProgramHeaderCount =
      Options.DynamicSource == LegacyDynamicTableSource::SectionHeader ? 1 : 2;
  const uint64_t TextOffset =
      llvm::alignTo(sizeof(Elf_Ehdr) + ProgramHeaderCount * sizeof(Elf_Phdr),
                    InstructionAlignment);
  const uint64_t StringTableOffset = TextOffset + Text.size();
  const uint64_t SymbolTableOffset = llvm::alignTo(
      StringTableOffset + StaticNames.size(), kELF64RecordAlignment);
  const uint64_t DynamicStringTableOffset =
      SymbolTableOffset + SymbolCount * sizeof(Elf_Sym);
  const uint64_t DynamicSymbolTableOffset = llvm::alignTo(
      DynamicStringTableOffset + DynamicNames.size(), kELF64RecordAlignment);

  size_t DynamicEntryCount = 2;
  if (Options.EmitDynamicRel)
    DynamicEntryCount += 3;
  if (Options.AddIgnoredDynamicRela)
    DynamicEntryCount += 3;
  const uint64_t DynamicOffset =
      llvm::alignTo(DynamicSymbolTableOffset + SymbolCount * sizeof(Elf_Sym),
                    kELF64RecordAlignment);
  const uint64_t RelocationOffset =
      DynamicOffset + DynamicEntryCount * sizeof(Elf_Dyn);
  const uint64_t RelocationSize = Options.Relocations.size() * sizeof(Elf_Rel);
  const uint64_t UnusedRelocationOffset =
      llvm::alignTo(RelocationOffset + RelocationSize, kELF64RecordAlignment);
  const uint64_t UnusedRelocationSize =
      Options.AddMalformedUnusedRel || Options.AddIgnoredDynamicRela
          ? sizeof(Elf_Rela)
          : 0;
  const uint64_t SectionNameTableOffset =
      UnusedRelocationOffset + UnusedRelocationSize;
  const uint64_t SectionTableOffset = llvm::alignTo(
      SectionNameTableOffset + SectionNames.size(), kELF64RecordAlignment);
  const uint64_t FileSize =
      SectionTableOffset + SectionCount * sizeof(Elf_Shdr);
  std::vector<uint8_t> Bytes(static_cast<size_t>(FileSize), 0);

  Elf_Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_ident[EI_OSABI] = ELFOSABI_NONE;
  Header.e_type = ET_DYN;
  Header.e_machine = kELFMachineBPF;
  Header.e_version = EV_CURRENT;
  Header.e_entry = TextOffset;
  Header.e_phoff = sizeof(Elf_Ehdr);
  Header.e_shoff = SectionTableOffset;
  Header.e_flags = static_cast<uint32_t>(Version::V0);
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_phentsize = sizeof(Elf_Phdr);
  Header.e_phnum = ProgramHeaderCount;
  Header.e_shentsize = sizeof(Elf_Shdr);
  Header.e_shnum = SectionCount;
  Header.e_shstrndx = SectionNameTableSectionIndex;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  std::array<Elf_Phdr, 2> ProgramHeaders{};
  ProgramHeaders[0].p_type = PT_LOAD;
  ProgramHeaders[0].p_flags = PF_R | PF_X;
  ProgramHeaders[0].p_offset = 0;
  ProgramHeaders[0].p_vaddr = 0;
  ProgramHeaders[0].p_paddr = 0;
  ProgramHeaders[0].p_filesz = SectionTableOffset;
  ProgramHeaders[0].p_memsz = SectionTableOffset;
  ProgramHeaders[0].p_align = InstructionAlignment;
  if (ProgramHeaderCount == 2) {
    ProgramHeaders[1].p_type = PT_DYNAMIC;
    ProgramHeaders[1].p_flags = PF_R;
    ProgramHeaders[1].p_offset = DynamicOffset;
    ProgramHeaders[1].p_vaddr = DynamicOffset;
    ProgramHeaders[1].p_paddr = DynamicOffset;
    const uint64_t DynamicSegmentSize = DynamicEntryCount * sizeof(Elf_Dyn);
    ProgramHeaders[1].p_filesz =
        Options.DynamicSource == LegacyDynamicTableSource::
                                     InvalidProgramHeaderWithSectionFallback
            ? DynamicSegmentSize - 1
            : DynamicSegmentSize;
    ProgramHeaders[1].p_memsz = ProgramHeaders[1].p_filesz;
    ProgramHeaders[1].p_align = kELF64RecordAlignment;
  }
  std::memcpy(Bytes.data() + Header.e_phoff, ProgramHeaders.data(),
              ProgramHeaderCount * sizeof(Elf_Phdr));

  std::memcpy(Bytes.data() + TextOffset, Text.data(), Text.size());
  std::memcpy(Bytes.data() + StringTableOffset, StaticNames.data(),
              StaticNames.size());
  std::memcpy(Bytes.data() + DynamicStringTableOffset, DynamicNames.data(),
              DynamicNames.size());
  std::memcpy(Bytes.data() + SectionNameTableOffset, SectionNames.data(),
              SectionNames.size());

  std::array<Elf_Sym, SymbolCount> StaticSymbols{};
  (void)NullSymbolIndex;
  if (Options.AddSameNamedStaticFunction) {
    StaticSymbols[BoundSymbolIndex].st_name = StaticSymbolNameOffset;
    StaticSymbols[BoundSymbolIndex].setBindingAndType(STB_GLOBAL, STT_FUNC);
    StaticSymbols[BoundSymbolIndex].st_shndx = TextSectionIndex;
    StaticSymbols[BoundSymbolIndex].st_value = TextOffset;
    StaticSymbols[BoundSymbolIndex].st_size = Text.size();
  }
  std::memcpy(Bytes.data() + SymbolTableOffset, StaticSymbols.data(),
              sizeof(StaticSymbols));

  std::array<Elf_Sym, SymbolCount> DynamicSymbols{};
  DynamicSymbols[BoundSymbolIndex].st_name = DynamicSymbolNameOffset;
  DynamicSymbols[BoundSymbolIndex].setBindingAndType(
      Options.DynamicSymbolBinding, Options.DynamicSymbolType);
  DynamicSymbols[BoundSymbolIndex].st_shndx = Options.DynamicSymbolSectionIndex;
  DynamicSymbols[BoundSymbolIndex].st_value = Options.DynamicSymbolValue;
  DynamicSymbols[BoundSymbolIndex].st_size = Options.DynamicSymbolSize;
  std::memcpy(Bytes.data() + DynamicSymbolTableOffset, DynamicSymbols.data(),
              sizeof(DynamicSymbols));

  std::vector<Elf_Rel> Relocations(Options.Relocations.size());
  for (size_t Index = 0; Index < Options.Relocations.size(); ++Index) {
    Relocations[Index].r_offset = Options.Relocations[Index].RawOffset.value_or(
        TextOffset + Options.Relocations[Index].TextByteOffset);
    Relocations[Index].setSymbolAndType(
        Options.Relocations[Index].SymbolIndex,
        static_cast<uint32_t>(Options.Relocations[Index].Type), false);
  }
  if (!Relocations.empty())
    std::memcpy(Bytes.data() + RelocationOffset, Relocations.data(),
                Relocations.size() * sizeof(Elf_Rel));

  std::vector<Elf_Dyn> DynamicEntries;
  auto AddDynamicEntry = [&](int64_t Tag, uint64_t Value) {
    Elf_Dyn Entry{};
    Entry.d_tag = Tag;
    Entry.d_un.d_val = Value;
    DynamicEntries.push_back(Entry);
  };
  AddDynamicEntry(DT_SYMTAB, DynamicSymbolTableOffset);
  if (Options.EmitDynamicRel) {
    constexpr uint64_t DeliberateMisalignment = 1;
    AddDynamicEntry(DT_REL, RelocationOffset + (Options.MisalignDynamicRel
                                                    ? DeliberateMisalignment
                                                    : 0));
    AddDynamicEntry(DT_RELSZ, RelocationSize);
    AddDynamicEntry(DT_RELENT, sizeof(Elf_Rel));
  }
  if (Options.AddIgnoredDynamicRela) {
    AddDynamicEntry(DT_RELA, UnusedRelocationOffset);
    AddDynamicEntry(DT_RELASZ, sizeof(Elf_Rela));
    AddDynamicEntry(DT_RELAENT, sizeof(Elf_Rela));
  }
  AddDynamicEntry(DT_NULL, 0);
  std::memcpy(Bytes.data() + DynamicOffset, DynamicEntries.data(),
              DynamicEntries.size() * sizeof(Elf_Dyn));

  std::array<Elf_Shdr, SectionCount> Sections{};
  (void)NullSectionIndex;
  Sections[TextSectionIndex].sh_name = TextName;
  Sections[TextSectionIndex].sh_type = SHT_PROGBITS;
  Sections[TextSectionIndex].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[TextSectionIndex].sh_addr = TextOffset;
  Sections[TextSectionIndex].sh_offset = TextOffset;
  Sections[TextSectionIndex].sh_size = Text.size();
  Sections[TextSectionIndex].sh_addralign = InstructionAlignment;

  Sections[StringTableSectionIndex].sh_name = StringTableName;
  Sections[StringTableSectionIndex].sh_type = SHT_STRTAB;
  Sections[StringTableSectionIndex].sh_offset = StringTableOffset;
  Sections[StringTableSectionIndex].sh_size = StaticNames.size();
  Sections[StringTableSectionIndex].sh_addralign = 1;

  Sections[SymbolTableSectionIndex].sh_name = SymbolTableName;
  Sections[SymbolTableSectionIndex].sh_type = SHT_SYMTAB;
  Sections[SymbolTableSectionIndex].sh_offset = SymbolTableOffset;
  Sections[SymbolTableSectionIndex].sh_size =
      sizeof(StaticSymbols) - (Options.MalformStaticSymbolTableSize ? 1 : 0);
  Sections[SymbolTableSectionIndex].sh_link = StringTableSectionIndex;
  Sections[SymbolTableSectionIndex].sh_info = BoundSymbolIndex;
  Sections[SymbolTableSectionIndex].sh_addralign = kELF64RecordAlignment;
  Sections[SymbolTableSectionIndex].sh_entsize = sizeof(Elf_Sym);

  Sections[DynamicStringTableSectionIndex].sh_name = DynamicStringTableName;
  Sections[DynamicStringTableSectionIndex].sh_type = SHT_STRTAB;
  Sections[DynamicStringTableSectionIndex].sh_addr = DynamicStringTableOffset;
  Sections[DynamicStringTableSectionIndex].sh_offset = DynamicStringTableOffset;
  Sections[DynamicStringTableSectionIndex].sh_size = DynamicNames.size();
  Sections[DynamicStringTableSectionIndex].sh_addralign = 1;

  Sections[DynamicSymbolTableSectionIndex].sh_name = DynamicSymbolTableName;
  Sections[DynamicSymbolTableSectionIndex].sh_type = SHT_DYNSYM;
  Sections[DynamicSymbolTableSectionIndex].sh_addr = DynamicSymbolTableOffset;
  Sections[DynamicSymbolTableSectionIndex].sh_offset = DynamicSymbolTableOffset;
  Sections[DynamicSymbolTableSectionIndex].sh_size = sizeof(DynamicSymbols);
  Sections[DynamicSymbolTableSectionIndex].sh_link =
      DynamicStringTableSectionIndex;
  Sections[DynamicSymbolTableSectionIndex].sh_info = BoundSymbolIndex;
  Sections[DynamicSymbolTableSectionIndex].sh_addralign = kELF64RecordAlignment;
  Sections[DynamicSymbolTableSectionIndex].sh_entsize = sizeof(Elf_Sym);

  Sections[DynamicSectionIndex].sh_name = DynamicName;
  Sections[DynamicSectionIndex].sh_type = SHT_DYNAMIC;
  Sections[DynamicSectionIndex].sh_addr = DynamicOffset;
  Sections[DynamicSectionIndex].sh_offset = DynamicOffset;
  Sections[DynamicSectionIndex].sh_size =
      DynamicEntries.size() * sizeof(Elf_Dyn);
  Sections[DynamicSectionIndex].sh_link = DynamicStringTableSectionIndex;
  Sections[DynamicSectionIndex].sh_addralign = kELF64RecordAlignment;
  Sections[DynamicSectionIndex].sh_entsize = sizeof(Elf_Dyn);

  Sections[RelocationSectionIndex].sh_type = SHT_REL;
  Sections[RelocationSectionIndex].sh_addr = RelocationOffset;
  Sections[RelocationSectionIndex].sh_offset = RelocationOffset;
  Sections[RelocationSectionIndex].sh_size = RelocationSize;
  Sections[RelocationSectionIndex].sh_link = DynamicSymbolTableSectionIndex;
  Sections[RelocationSectionIndex].sh_info = TextSectionIndex;
  Sections[RelocationSectionIndex].sh_addralign = kELF64RecordAlignment;
  Sections[RelocationSectionIndex].sh_entsize = sizeof(Elf_Rel);

  Sections[UnusedRelocationSectionIndex].sh_type =
      Options.AddIgnoredDynamicRela ? SHT_RELA : SHT_REL;
  switch (Options.DuplicateMetadataSection) {
  case LegacyDuplicateMetadataSection::None:
    break;
  case LegacyDuplicateMetadataSection::Symtab:
    Sections[UnusedRelocationSectionIndex].sh_name = SymbolTableName;
    break;
  case LegacyDuplicateMetadataSection::Strtab:
    Sections[UnusedRelocationSectionIndex].sh_name = StringTableName;
    break;
  case LegacyDuplicateMetadataSection::Dynstr:
    Sections[UnusedRelocationSectionIndex].sh_name = DynamicStringTableName;
    break;
  }
  Sections[UnusedRelocationSectionIndex].sh_addr = UnusedRelocationOffset;
  Sections[UnusedRelocationSectionIndex].sh_offset = UnusedRelocationOffset;
  Sections[UnusedRelocationSectionIndex].sh_size = UnusedRelocationSize;
  Sections[UnusedRelocationSectionIndex].sh_link =
      DynamicSymbolTableSectionIndex;
  Sections[UnusedRelocationSectionIndex].sh_info = TextSectionIndex;
  Sections[UnusedRelocationSectionIndex].sh_addralign = kELF64RecordAlignment;
  Sections[UnusedRelocationSectionIndex].sh_entsize =
      Options.AddMalformedUnusedRel ? sizeof(Elf_Rel) - 1 : sizeof(Elf_Rela);

  Sections[SectionNameTableSectionIndex].sh_name = SectionNameTableName;
  Sections[SectionNameTableSectionIndex].sh_type = SHT_STRTAB;
  Sections[SectionNameTableSectionIndex].sh_offset = SectionNameTableOffset;
  Sections[SectionNameTableSectionIndex].sh_size = SectionNames.size();
  Sections[SectionNameTableSectionIndex].sh_addralign = 1;

  std::memcpy(Bytes.data() + SectionTableOffset, Sections.data(),
              sizeof(Sections));
  return Bytes;
}

inline std::vector<uint8_t>
buildStrictELF(const StrictELFOptions &Options = {}) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Phdr = ELFT::Phdr;
  using Elf_Shdr = ELFT::Shdr;
  using Elf_Sym = ELFT::Sym;
  using namespace llvm::ELF;

  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  if (!Exit)
    return {};
  std::array<uint8_t, kInstructionSize> DefaultText{};
  DefaultText[kOpcodeOffset] = Exit->Encoding;
  const llvm::ArrayRef<uint8_t> Text =
      Options.Text.empty() ? llvm::ArrayRef<uint8_t>(DefaultText)
                           : llvm::ArrayRef<uint8_t>(Options.Text);
  std::array<uint8_t, kInstructionSize> Rodata{};

  const size_t ProgramHeaderCount = Options.AddRodata ? 2 : 1;
  const uint64_t SegmentOffset =
      sizeof(Elf_Ehdr) + ProgramHeaderCount * sizeof(Elf_Phdr);
  const uint64_t TextOffset =
      SegmentOffset + (Options.AddRodata ? Rodata.size() : 0);
  const uint64_t RuntimeEnd = TextOffset + Text.size();

  static constexpr char SectionNameData[] = "\0.shstrtab\0.strtab\0.symtab\0";
  constexpr std::string_view SectionNames(SectionNameData,
                                          sizeof(SectionNameData) - 1);
  std::vector<StrictELFFunctionSymbol> FunctionSymbols =
      Options.FunctionSymbols;
  if (FunctionSymbols.empty())
    FunctionSymbols.push_back({Options.DebugSymbolName, Options.EntrySlot,
                               Text.size() / kInstructionSize});
  std::string SymbolNames(1, '\0');
  std::vector<uint32_t> SymbolNameOffsets;
  SymbolNameOffsets.reserve(FunctionSymbols.size());
  for (const StrictELFFunctionSymbol &Symbol : FunctionSymbols) {
    SymbolNameOffsets.push_back(static_cast<uint32_t>(SymbolNames.size()));
    SymbolNames.append(Symbol.Name);
    SymbolNames.push_back('\0');
  }
  const size_t SymbolCount = FunctionSymbols.size() + 1;
  constexpr uint32_t ShStrTabName = 1;
  constexpr uint32_t StrTabName = ShStrTabName + sizeof(".shstrtab");
  constexpr uint32_t SymTabName = StrTabName + sizeof(".strtab");
  constexpr size_t DebugSectionCount = 4;
  const uint64_t SectionNamesOffset = RuntimeEnd;
  const uint64_t SymbolNamesOffset = SectionNamesOffset + SectionNames.size();
  const uint64_t SymbolTableOffset = llvm::alignTo(
      SymbolNamesOffset + SymbolNames.size(), kELF64RecordAlignment);
  const uint64_t SectionTableOffset = llvm::alignTo(
      SymbolTableOffset + SymbolCount * sizeof(Elf_Sym), kELF64RecordAlignment);
  const uint64_t FileSize =
      Options.AddDebugSymbols
          ? SectionTableOffset + DebugSectionCount * sizeof(Elf_Shdr)
          : RuntimeEnd;
  std::vector<uint8_t> Bytes(static_cast<size_t>(FileSize), 0);

  Elf_Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_ident[EI_OSABI] = ELFOSABI_NONE;
  Header.e_type = ET_DYN;
  Header.e_machine = Options.Machine;
  Header.e_version = EV_CURRENT;
  Header.e_entry = kBytecodeStart + Options.EntrySlot * kInstructionSize;
  Header.e_phoff = sizeof(Elf_Ehdr);
  Header.e_flags = static_cast<uint32_t>(Options.TheVersion);
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_phentsize = sizeof(Elf_Phdr);
  Header.e_phnum = ProgramHeaderCount;
  Header.e_shentsize = sizeof(ELFT::Shdr);
  Header.e_shstrndx = SHN_UNDEF;
  if (Options.AddDebugSymbols) {
    Header.e_shoff = SectionTableOffset;
    Header.e_shnum = DebugSectionCount;
    Header.e_shstrndx = 1;
  }
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  std::array<Elf_Phdr, 2> ProgramHeaders{};
  size_t TextHeaderIndex = 0;
  if (Options.AddRodata) {
    Elf_Phdr &RodataHeader = ProgramHeaders[0];
    RodataHeader.p_type = PT_LOAD;
    RodataHeader.p_flags = PF_R;
    RodataHeader.p_offset = SegmentOffset;
    RodataHeader.p_vaddr = kRodataStartV3;
    RodataHeader.p_paddr = kRodataStartV3;
    RodataHeader.p_filesz = Rodata.size();
    RodataHeader.p_memsz = Rodata.size();
    RodataHeader.p_align = kInstructionSize;
    TextHeaderIndex = 1;
    std::memcpy(Bytes.data() + SegmentOffset, Rodata.data(), Rodata.size());
  }
  Elf_Phdr &TextHeader = ProgramHeaders[TextHeaderIndex];
  TextHeader.p_type = PT_LOAD;
  TextHeader.p_flags = PF_X;
  TextHeader.p_offset = TextOffset;
  TextHeader.p_vaddr = kBytecodeStart;
  TextHeader.p_paddr = kBytecodeStart;
  TextHeader.p_filesz = Text.size();
  TextHeader.p_memsz = Text.size();
  TextHeader.p_align = kInstructionSize;
  std::memcpy(Bytes.data() + sizeof(Elf_Ehdr), ProgramHeaders.data(),
              ProgramHeaderCount * sizeof(Elf_Phdr));
  std::memcpy(Bytes.data() + TextOffset, Text.data(), Text.size());

  if (Options.AddDebugSymbols) {
    std::memcpy(Bytes.data() + SectionNamesOffset, SectionNames.data(),
                SectionNames.size());
    std::memcpy(Bytes.data() + SymbolNamesOffset, SymbolNames.data(),
                SymbolNames.size());

    std::vector<Elf_Sym> Symbols(SymbolCount);
    for (size_t SymbolID = 0; SymbolID < FunctionSymbols.size(); ++SymbolID) {
      Elf_Sym &Symbol = Symbols[SymbolID + 1];
      Symbol.st_name = SymbolNameOffsets[SymbolID];
      Symbol.setBindingAndType(STB_GLOBAL, STT_FUNC);
      Symbol.st_shndx = SHN_ABS;
      Symbol.st_value = kBytecodeStart +
                        FunctionSymbols[SymbolID].EntrySlot * kInstructionSize;
      Symbol.st_size = FunctionSymbols[SymbolID].SlotCount * kInstructionSize;
    }
    std::memcpy(Bytes.data() + SymbolTableOffset, Symbols.data(),
                Symbols.size() * sizeof(Elf_Sym));

    std::array<Elf_Shdr, DebugSectionCount> Sections{};
    Sections[1].sh_name = ShStrTabName;
    Sections[1].sh_type = SHT_STRTAB;
    Sections[1].sh_offset = SectionNamesOffset;
    Sections[1].sh_size = SectionNames.size();
    Sections[1].sh_addralign = 1;
    Sections[2].sh_name = StrTabName;
    Sections[2].sh_type = SHT_STRTAB;
    Sections[2].sh_offset = SymbolNamesOffset;
    Sections[2].sh_size = SymbolNames.size();
    Sections[2].sh_addralign = 1;
    Sections[3].sh_name = SymTabName;
    Sections[3].sh_type = SHT_SYMTAB;
    Sections[3].sh_offset = SymbolTableOffset;
    Sections[3].sh_size = Symbols.size() * sizeof(Elf_Sym);
    Sections[3].sh_link = 2;
    Sections[3].sh_info = 1;
    Sections[3].sh_addralign = kELF64RecordAlignment;
    Sections[3].sh_entsize = sizeof(Elf_Sym);
    std::memcpy(Bytes.data() + SectionTableOffset, Sections.data(),
                sizeof(Sections));
  }
  return Bytes;
}

} // namespace neverd::sbf::test

#endif // NEVERD_UNITTESTS_SBF_SBFFIXTUREBUILDER_H
