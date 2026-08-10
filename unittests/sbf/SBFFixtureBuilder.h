//===- SBFFixtureBuilder.h - Deterministic Solana SBF ELF fixtures -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SBF_SBFFIXTUREBUILDER_H
#define NEVERD_UNITTESTS_SBF_SBFFIXTUREBUILDER_H

#include "neverd/sbf/Opcodes.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/Version.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/MathExtras.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace neverd::sbf::test {

struct LegacyELFOptions {
  Version TheVersion = Version::V0;
  uint16_t Machine = kELFMachineBPF;
  bool AddWritableData = false;
};

struct StrictELFOptions {
  Version TheVersion = Version::V3;
  uint16_t Machine = kELFMachineBPF;
  bool AddRodata = false;
  size_t EntrySlot = 0;
  std::vector<uint8_t> Text;
};

inline std::vector<uint8_t>
buildLegacyELF(const LegacyELFOptions &Options = {}) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Shdr = ELFT::Shdr;
  using namespace llvm::ELF;

  static constexpr char SectionNameData[] = "\0.text\0.data\0.shstrtab\0";
  constexpr std::string_view SectionNames(SectionNameData,
                                          sizeof(SectionNameData) - 1);
  constexpr uint32_t TextName = 1;
  constexpr uint32_t DataName = TextName + sizeof(".text");
  constexpr uint32_t StringTableName = DataName + sizeof(".data");
  constexpr uint64_t SectionAlignment = kInstructionSize;
  constexpr size_t NullSectionIndex = 0;
  constexpr size_t TextSectionIndex = 1;
  constexpr size_t DataSectionIndex = 2;
  constexpr size_t StringTableSectionIndex = 3;
  constexpr size_t SectionCount = 4;

  std::array<uint8_t, kInstructionSize> Text{};
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  if (!Exit)
    return {};
  Text[kOpcodeOffset] = Exit->Encoding;
  std::array<uint8_t, kInstructionSize> Data{};

  const uint64_t TextOffset = llvm::alignTo(sizeof(Elf_Ehdr), SectionAlignment);
  const uint64_t DataOffset = TextOffset + Text.size();
  const uint64_t StringTableOffset =
      DataOffset + (Options.AddWritableData ? Data.size() : 0);
  const uint64_t SectionTableOffset =
      llvm::alignTo(StringTableOffset + SectionNames.size(), alignof(Elf_Shdr));
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
  Header.e_entry = 0;
  Header.e_shoff = SectionTableOffset;
  Header.e_flags = static_cast<uint32_t>(Options.TheVersion);
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_phentsize = sizeof(ELFT::Phdr);
  Header.e_shentsize = sizeof(Elf_Shdr);
  Header.e_shnum = SectionCount;
  Header.e_shstrndx = StringTableSectionIndex;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  std::memcpy(Bytes.data() + TextOffset, Text.data(), Text.size());
  if (Options.AddWritableData)
    std::memcpy(Bytes.data() + DataOffset, Data.data(), Data.size());
  std::memcpy(Bytes.data() + StringTableOffset, SectionNames.data(),
              SectionNames.size());

  std::array<Elf_Shdr, SectionCount> Sections{};
  (void)NullSectionIndex;
  Sections[TextSectionIndex].sh_name = TextName;
  Sections[TextSectionIndex].sh_type = SHT_PROGBITS;
  Sections[TextSectionIndex].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[TextSectionIndex].sh_addr = 0;
  Sections[TextSectionIndex].sh_offset = TextOffset;
  Sections[TextSectionIndex].sh_size = Text.size();
  Sections[TextSectionIndex].sh_addralign = SectionAlignment;

  Sections[DataSectionIndex].sh_name = DataName;
  Sections[DataSectionIndex].sh_type = SHT_PROGBITS;
  Sections[DataSectionIndex].sh_flags = SHF_ALLOC | SHF_WRITE;
  Sections[DataSectionIndex].sh_addr = Text.size();
  Sections[DataSectionIndex].sh_offset = DataOffset;
  Sections[DataSectionIndex].sh_size =
      Options.AddWritableData ? Data.size() : 0;
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

inline std::vector<uint8_t>
buildStrictELF(const StrictELFOptions &Options = {}) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Phdr = ELFT::Phdr;
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
  std::vector<uint8_t> Bytes(static_cast<size_t>(TextOffset + Text.size()), 0);

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
  return Bytes;
}

} // namespace neverd::sbf::test

#endif // NEVERD_UNITTESTS_SBF_SBFFIXTUREBUILDER_H
