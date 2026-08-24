//===- SBFELFLoader.cpp - Solana SBF ELF loader -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/SBFELFLoader.h"

#include "neverd/object/SectionNames.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/image/SBFVersion.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace neverd {
namespace {

using ELFT = llvm::object::ELF64LE;
using ELFFile = llvm::object::ELFFile<ELFT>;
using Elf_Ehdr = ELFT::Ehdr;
using Elf_Phdr = ELFT::Phdr;
using Elf_Shdr = ELFT::Shdr;
using Elf_Sym = ELFT::Sym;
using Elf_Dyn = ELFT::Dyn;
using Elf_Rel = ELFT::Rel;

llvm::Error sbfError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(("sbf: " + Message).str(),
                                             llvm::inconvertibleErrorCode());
}

bool hasSBFMachine(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < sizeof(Elf_Ehdr))
    return false;
  if (!std::equal(Bytes.begin(), Bytes.begin() + llvm::ELF::EI_MAG3 + 1,
                  reinterpret_cast<const uint8_t *>(llvm::ELF::ElfMagic)))
    return false;
  if (Bytes[llvm::ELF::EI_CLASS] != llvm::ELF::ELFCLASS64 ||
      Bytes[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2LSB)
    return false;
  constexpr size_t MachineOffset = offsetof(Elf_Ehdr, e_machine);
  const uint16_t Machine =
      llvm::support::endian::read16le(Bytes.data() + MachineOffset);
  return Machine == sbf::kELFMachineBPF || Machine == sbf::kELFMachineSBPF;
}

llvm::Expected<ELFFile> parseELF(const BinaryImage &Image) {
  llvm::StringRef Bytes(reinterpret_cast<const char *>(Image.Raw.data()),
                        Image.Raw.size());
  return ELFFile::create(Bytes);
}

bool checkedAdd(uint64_t L, uint64_t R, uint64_t &Result) {
  if (R > std::numeric_limits<uint64_t>::max() - L)
    return false;
  Result = L + R;
  return true;
}

bool checkedMultiply(uint64_t L, uint64_t R, uint64_t &Result) {
  if (L != 0 && R > std::numeric_limits<uint64_t>::max() / L)
    return false;
  Result = L * R;
  return true;
}

llvm::Expected<llvm::StringRef>
readBoundedELFString(llvm::StringRef Table, uint64_t Offset, uint64_t ByteLimit,
                     llvm::StringRef Description) {
  if (Offset >= Table.size())
    return sbfError(llvm::Twine(Description) + " offset is out of bounds");
  const llvm::StringRef Window =
      Table.drop_front(static_cast<size_t>(Offset))
          .take_front(static_cast<size_t>(ByteLimit));
  const size_t Terminator = Window.find('\0');
  if (Terminator == llvm::StringRef::npos)
    return sbfError(llvm::Twine(Description) + " exceeds the " +
                    llvm::Twine(ByteLimit) + "-byte limit");
  return Window.take_front(Terminator);
}

struct LegacySectionCatalog {
  std::vector<llvm::StringRef> Names;
  const Elf_Shdr *SymbolTable = nullptr;
  const Elf_Shdr *SymbolNames = nullptr;
  const Elf_Shdr *DynamicSymbolNames = nullptr;
};

llvm::Expected<LegacySectionCatalog>
parseLegacySectionCatalog(llvm::ArrayRef<Elf_Shdr> Sections,
                          llvm::StringRef StringTable) {
  LegacySectionCatalog Catalog;
  Catalog.Names.reserve(Sections.size());
  for (const Elf_Shdr &SH : Sections) {
    auto Name = readBoundedELFString(StringTable, SH.sh_name,
                                     sbf::kELFSectionNameByteLimit,
                                     "legacy ELF section name");
    if (!Name)
      return Name.takeError();
    Catalog.Names.push_back(*Name);

    const Elf_Shdr **Slot = nullptr;
    if (*Name == section_names::elf::Symtab)
      Slot = &Catalog.SymbolTable;
    else if (*Name == section_names::elf::Strtab)
      Slot = &Catalog.SymbolNames;
    else if (*Name == section_names::elf::Dynstr)
      Slot = &Catalog.DynamicSymbolNames;
    if (!Slot)
      continue;
    if (*Slot)
      return sbfError(llvm::Twine("legacy ELF contains multiple ") + *Name +
                      " sections");
    *Slot = &SH;
  }
  return Catalog;
}

bool isOfficialELF64RecordAlignedAt(llvm::ArrayRef<uint8_t> Bytes,
                                    uint64_t Offset) {
  if (Offset > Bytes.size())
    return false;
  const uintptr_t Address =
      reinterpret_cast<uintptr_t>(Bytes.data() + static_cast<size_t>(Offset));
  return Address % kELF64RecordAlignment == 0;
}

struct FileSpan {
  uint64_t Begin = 0;
  uint64_t End = 0;

  bool overlaps(const FileSpan &Other) const {
    return Begin < Other.End && Other.Begin < End;
  }
};

llvm::Expected<FileSpan> checkedFileSpan(uint64_t Offset, uint64_t Size,
                                         uint64_t FileSize,
                                         llvm::StringRef Description) {
  if (!rangeInBounds(Offset, Size, FileSize))
    return sbfError(llvm::Twine(Description) + " is out of file bounds");
  return FileSpan{Offset, Offset + Size};
}

llvm::Error validateLegacyParserLayout(const ELFFile &ELF,
                                       const Elf_Ehdr &Header,
                                       llvm::ArrayRef<Elf_Shdr> Sections,
                                       llvm::ArrayRef<uint8_t> Bytes) {
  using namespace llvm::ELF;
  if (Header.e_ehsize != sizeof(Elf_Ehdr) ||
      Header.e_phentsize != sizeof(Elf_Phdr) ||
      Header.e_shentsize != sizeof(Elf_Shdr) ||
      Header.e_shstrndx >= Header.e_shnum)
    return sbfError("invalid legacy ELF header layout");
  if (Sections.empty() || Sections.front().sh_type != SHT_NULL)
    return sbfError("legacy ELF section zero is not SHT_NULL");

  uint64_t ProgramHeaderTableSize = 0;
  uint64_t SectionHeaderTableSize = 0;
  if (!checkedMultiply(Header.e_phnum, sizeof(Elf_Phdr),
                       ProgramHeaderTableSize) ||
      !checkedMultiply(Header.e_shnum, sizeof(Elf_Shdr),
                       SectionHeaderTableSize))
    return sbfError("legacy ELF header table size overflows");
  auto FileHeader = checkedFileSpan(0, sizeof(Elf_Ehdr), Bytes.size(),
                                    "legacy ELF file header");
  auto ProgramHeaderTable =
      checkedFileSpan(Header.e_phoff, ProgramHeaderTableSize, Bytes.size(),
                      "legacy ELF program header table");
  auto SectionHeaderTable =
      checkedFileSpan(Header.e_shoff, SectionHeaderTableSize, Bytes.size(),
                      "legacy ELF section header table");
  if (!FileHeader)
    return FileHeader.takeError();
  if (!ProgramHeaderTable)
    return ProgramHeaderTable.takeError();
  if (!SectionHeaderTable)
    return SectionHeaderTable.takeError();
  if (!isOfficialELF64RecordAlignedAt(Bytes, 0) ||
      !isOfficialELF64RecordAlignedAt(Bytes, Header.e_phoff) ||
      !isOfficialELF64RecordAlignedAt(Bytes, Header.e_shoff))
    return sbfError("legacy ELF header table is misaligned");
  if (FileHeader->overlaps(*ProgramHeaderTable) ||
      FileHeader->overlaps(*SectionHeaderTable) ||
      ProgramHeaderTable->overlaps(*SectionHeaderTable))
    return sbfError("legacy ELF header tables overlap");

  auto ProgramHeaders = ELF.program_headers();
  if (!ProgramHeaders)
    return ProgramHeaders.takeError();
  uint64_t LastLoadAddress = 0;
  for (const Elf_Phdr &PH : *ProgramHeaders) {
    if (PH.p_type != PT_LOAD)
      continue;
    uint64_t LoadEnd = 0;
    if (PH.p_vaddr < LastLoadAddress ||
        !checkedAdd(PH.p_vaddr, PH.p_memsz, LoadEnd))
      return sbfError("legacy PT_LOAD program headers are not ascending");
    if (!rangeInBounds(PH.p_offset, PH.p_filesz, Bytes.size()))
      return sbfError("legacy PT_LOAD program header is out of file bounds");
    LastLoadAddress = PH.p_vaddr;
  }

  uint64_t PreviousSectionEnd = 0;
  for (const Elf_Shdr &SH : Sections) {
    if (SH.sh_type == SHT_NOBITS)
      continue;
    auto Section = checkedFileSpan(SH.sh_offset, SH.sh_size, Bytes.size(),
                                   "legacy ELF section");
    if (!Section)
      return Section.takeError();
    if (Section->overlaps(*FileHeader) ||
        Section->overlaps(*ProgramHeaderTable) ||
        Section->overlaps(*SectionHeaderTable))
      return sbfError("legacy ELF section overlaps a header table");
    if (Section->Begin < PreviousSectionEnd)
      return sbfError("legacy ELF sections are not in file order");
    PreviousSectionEnd = Section->End;
  }
  return llvm::Error::success();
}

va_t legacyRuntimeAddress(uint64_t Address) {
  uint64_t Result = 0;
  if (!checkedAdd(sbf::kBytecodeStart, Address, Result))
    return InvalidVA;
  return Result;
}

llvm::Expected<va_t> legacyTextRuntimeAddress(uint64_t Address, uint64_t Size) {
  uint64_t RuntimeAddress = 0;
  if (!checkedAdd(sbf::kBytecodeStart, Address, RuntimeAddress))
    return sbfError("legacy .text virtual address overflows");

  uint64_t RuntimeEnd = 0;
  if (!checkedAdd(RuntimeAddress, Size, RuntimeEnd) ||
      RuntimeAddress >= sbf::kStackStart || RuntimeEnd > sbf::kStackStart)
    return sbfError(
        "legacy .text virtual address range exceeds bytecode region");
  return RuntimeAddress;
}

llvm::Error validateIdentity(const Elf_Ehdr &Header, bool Strict) {
  using namespace llvm::ELF;
  if (Header.e_ident[EI_CLASS] != ELFCLASS64)
    return sbfError("ELF class must be ELF64");
  if (Header.e_ident[EI_DATA] != ELFDATA2LSB)
    return sbfError("ELF byte order must be little-endian");
  if (Header.e_ident[EI_VERSION] != EV_CURRENT)
    return sbfError("unsupported ELF identification version");
  if (Header.e_ident[EI_OSABI] != ELFOSABI_NONE)
    return sbfError("ELF OS ABI must be System V");
  if (Strict) {
    if (Header.e_ident[EI_ABIVERSION] != 0)
      return sbfError("strict ELF ABI version must be zero");
    for (unsigned I = EI_PAD; I < EI_NIDENT; ++I)
      if (Header.e_ident[I] != 0)
        return sbfError("strict ELF identification padding must be zero");
  }
  if (Header.e_version != EV_CURRENT)
    return sbfError("unsupported ELF header version");
  return llvm::Error::success();
}

void addMappedRegion(BinaryImage &Image, llvm::StringRef Name, va_t Address,
                     uint64_t FileOffset, uint64_t FileSize,
                     uint64_t MemorySize, SegmentFlags Flags,
                     uint32_t Alignment) {
  Segment Seg;
  Seg.Name = Name.str();
  Seg.VA = Address;
  Seg.Size = MemorySize;
  Seg.FileOff = FileOffset;
  Seg.FileSz = FileSize;
  Seg.Flags = Flags;
  Seg.Data.assign(Image.Raw.begin() + static_cast<ptrdiff_t>(FileOffset),
                  Image.Raw.begin() +
                      static_cast<ptrdiff_t>(FileOffset + FileSize));
  Image.Segments.push_back(Seg);

  Section Sec;
  Sec.Name = Name.str();
  Sec.SegmentName = Name.str();
  Sec.VA = Address;
  Sec.Size = MemorySize;
  Sec.FileOff = FileOffset;
  Sec.FileSz = FileSize;
  Sec.Flags = Flags;
  Sec.Alignment = Alignment;
  Sec.Data = std::move(Seg.Data);
  Image.Sections.push_back(std::move(Sec));
}

sbf::DebugEnrichmentStatus
addStrictDebugSymbols(llvm::ArrayRef<Elf_Shdr> Sections,
                      llvm::StringRef SectionNames,
                      llvm::ArrayRef<uint8_t> Bytes, BinaryImage &Image) {
  using namespace llvm::ELF;
  const Elf_Shdr *SymbolTable = nullptr;
  const Elf_Shdr *SymbolNameTable = nullptr;
  bool SawMalformedName = false;
  for (const Elf_Shdr &SH : Sections) {
    auto Name = readBoundedELFString(SectionNames, SH.sh_name,
                                     sbf::kELFSectionNameByteLimit,
                                     "strict ELF debug section name");
    if (!Name) {
      llvm::consumeError(Name.takeError());
      SawMalformedName = true;
      continue;
    }
    if (*Name == section_names::elf::Symtab)
      SymbolTable = &SH;
    else if (*Name == section_names::elf::Strtab)
      SymbolNameTable = &SH;
  }
  if (!SymbolTable || !SymbolNameTable)
    return SawMalformedName ? sbf::DebugEnrichmentStatus::Malformed
                            : sbf::DebugEnrichmentStatus::Unavailable;
  if (SymbolTable->sh_size % sizeof(Elf_Sym) != 0 ||
      !rangeInBounds(SymbolTable->sh_offset, SymbolTable->sh_size,
                     Bytes.size()) ||
      !isOfficialELF64RecordAlignedAt(Bytes, SymbolTable->sh_offset) ||
      SymbolNameTable->sh_type != SHT_STRTAB ||
      !rangeInBounds(SymbolNameTable->sh_offset, SymbolNameTable->sh_size,
                     Bytes.size()))
    return sbf::DebugEnrichmentStatus::Malformed;

  const llvm::StringRef SymbolNames(
      reinterpret_cast<const char *>(Bytes.data() + SymbolNameTable->sh_offset),
      static_cast<size_t>(SymbolNameTable->sh_size));
  const size_t Count =
      static_cast<size_t>(SymbolTable->sh_size / sizeof(Elf_Sym));
  for (size_t Index = 0; Index < Count; ++Index) {
    const uint64_t Offset =
        SymbolTable->sh_offset + uint64_t{Index} * sizeof(Elf_Sym);
    Elf_Sym RawSymbol;
    std::memcpy(&RawSymbol, Bytes.data() + Offset, sizeof(RawSymbol));
    if (RawSymbol.st_name == 0)
      continue;
    auto Name = readBoundedELFString(SymbolNames, RawSymbol.st_name,
                                     sbf::kELFDebugSymbolNameByteLimit,
                                     "strict ELF debug symbol name");
    if (!Name) {
      llvm::consumeError(Name.takeError());
      SawMalformedName = true;
      continue;
    }
    if (Name->empty())
      continue;

    Symbol Symbol;
    Symbol.Name = Name->str();
    Symbol.Addr = RawSymbol.st_value;
    Symbol.Size = RawSymbol.st_size;
    Symbol.IsFunc = RawSymbol.getType() == STT_FUNC;
    Image.Symbols.push_back(std::move(Symbol));
  }
  return SawMalformedName ? sbf::DebugEnrichmentStatus::Malformed
                          : sbf::DebugEnrichmentStatus::Complete;
}

sbf::DebugEnrichmentStatus
addLegacyDebugSymbols(const LegacySectionCatalog &Catalog,
                      llvm::ArrayRef<uint8_t> Bytes, sbf::Version Version,
                      BinaryImage &Image) {
  using namespace llvm::ELF;
  const Elf_Shdr *SymbolTable = Catalog.SymbolTable;
  if (!SymbolTable || !Catalog.SymbolNames)
    return sbf::DebugEnrichmentStatus::Unavailable;
  if ((SymbolTable->sh_type != SHT_SYMTAB &&
       SymbolTable->sh_type != SHT_DYNSYM) ||
      SymbolTable->sh_size % sizeof(Elf_Sym) != 0 ||
      !rangeInBounds(SymbolTable->sh_offset, SymbolTable->sh_size,
                     Bytes.size()) ||
      !isOfficialELF64RecordAlignedAt(Bytes, SymbolTable->sh_offset))
    return sbf::DebugEnrichmentStatus::Malformed;

  llvm::StringRef SymbolNames;
  if (Catalog.SymbolNames && Catalog.SymbolNames->sh_type == SHT_STRTAB &&
      rangeInBounds(Catalog.SymbolNames->sh_offset,
                    Catalog.SymbolNames->sh_size, Bytes.size())) {
    SymbolNames =
        llvm::StringRef(reinterpret_cast<const char *>(
                            Bytes.data() + Catalog.SymbolNames->sh_offset),
                        static_cast<size_t>(Catalog.SymbolNames->sh_size));
  } else
    return sbf::DebugEnrichmentStatus::Malformed;

  bool SawMalformedLabel = false;
  const size_t Count =
      static_cast<size_t>(SymbolTable->sh_size / sizeof(Elf_Sym));
  for (size_t Index = 0; Index < Count; ++Index) {
    const uint64_t Offset =
        SymbolTable->sh_offset + uint64_t{Index} * sizeof(Elf_Sym);
    Elf_Sym RawSymbol;
    std::memcpy(&RawSymbol, Bytes.data() + Offset, sizeof(RawSymbol));
    if (RawSymbol.st_name == 0)
      continue;
    if (SymbolNames.empty()) {
      SawMalformedLabel = true;
      continue;
    }

    auto Name = readBoundedELFString(SymbolNames, RawSymbol.st_name,
                                     sbf::kELFDebugSymbolNameByteLimit,
                                     "legacy debug symbol name");
    if (!Name) {
      llvm::consumeError(Name.takeError());
      SawMalformedLabel = true;
      continue;
    }
    if (Name->empty())
      continue;

    va_t Address = RawSymbol.st_value;
    if (!sbf::versionHasFeature(Version, sbf::VersionFeature::StrictELF) &&
        RawSymbol.st_shndx != SHN_UNDEF) {
      Address = legacyRuntimeAddress(Address);
      if (Address == InvalidVA)
        continue;
    }

    Symbol Symbol;
    Symbol.Name = Name->str();
    Symbol.Addr = Address;
    Symbol.Size = RawSymbol.st_size;
    Symbol.IsFunc = RawSymbol.getType() == STT_FUNC;
    Image.Symbols.push_back(std::move(Symbol));
  }
  return SawMalformedLabel ? sbf::DebugEnrichmentStatus::Malformed
                           : sbf::DebugEnrichmentStatus::Complete;
}

struct LegacyDynamicTable {
  ELFRelocationSource Source = ELFRelocationSource::ProgramDynamicTable;
  uint64_t FileOffset = 0;
  uint64_t Size = 0;
};

struct LegacyDynamicTags {
  uint64_t RelocationAddress = 0;
  uint64_t RelocationSize = 0;
  uint64_t RelocationEntrySize = 0;
  uint64_t SymbolTableAddress = 0;
};

struct DynamicSymbolTable {
  const Elf_Shdr *Header = nullptr;
  uint32_t SectionIndex = 0;
};

llvm::Expected<std::optional<LegacyDynamicTable>>
findLegacyDynamicTable(const ELFFile &ELF, llvm::ArrayRef<Elf_Shdr> Sections,
                       llvm::ArrayRef<uint8_t> Bytes) {
  using namespace llvm::ELF;
  auto ProgramHeaders = ELF.program_headers();
  if (!ProgramHeaders)
    return ProgramHeaders.takeError();

  // Latest sbpf first tries PT_DYNAMIC. An invalid segment payload does not
  // make the file terminally invalid here: historical fixtures require the
  // same SHT_DYNAMIC fallback as the official parser.
  for (const Elf_Phdr &PH : *ProgramHeaders) {
    if (PH.p_type != PT_DYNAMIC)
      continue;
    if (PH.p_filesz % sizeof(Elf_Dyn) == 0 &&
        rangeInBounds(PH.p_offset, PH.p_filesz, Bytes.size()) &&
        isOfficialELF64RecordAlignedAt(Bytes, PH.p_offset))
      return LegacyDynamicTable{ELFRelocationSource::ProgramDynamicTable,
                                PH.p_offset, PH.p_filesz};
    break;
  }

  for (const Elf_Shdr &SH : Sections) {
    if (SH.sh_type != SHT_DYNAMIC)
      continue;
    if (SH.sh_size % sizeof(Elf_Dyn) != 0 ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Bytes.size()) ||
        !isOfficialELF64RecordAlignedAt(Bytes, SH.sh_offset))
      return sbfError("invalid legacy dynamic table");
    return LegacyDynamicTable{ELFRelocationSource::SectionDynamicTable,
                              SH.sh_offset, SH.sh_size};
  }
  return std::nullopt;
}

LegacyDynamicTags readLegacyDynamicTags(const LegacyDynamicTable &Table,
                                        llvm::ArrayRef<uint8_t> Bytes) {
  using namespace llvm::ELF;
  LegacyDynamicTags Tags;
  const size_t Count = static_cast<size_t>(Table.Size / sizeof(Elf_Dyn));
  for (size_t Index = 0; Index < Count; ++Index) {
    Elf_Dyn Entry;
    std::memcpy(&Entry,
                Bytes.data() + Table.FileOffset + Index * sizeof(Elf_Dyn),
                sizeof(Entry));
    if (Entry.getTag() == DT_NULL)
      break;
    switch (Entry.getTag()) {
    case DT_REL:
      Tags.RelocationAddress = Entry.getPtr();
      break;
    case DT_RELSZ:
      Tags.RelocationSize = Entry.getVal();
      break;
    case DT_RELENT:
      Tags.RelocationEntrySize = Entry.getVal();
      break;
    case DT_SYMTAB:
      Tags.SymbolTableAddress = Entry.getPtr();
      break;
    default:
      // In particular, DT_RELA/DT_RELASZ/DT_RELAENT are not part of the
      // legacy SBF runtime relocation inventory.
      break;
    }
  }
  return Tags;
}

llvm::Expected<uint64_t> fileOffsetForVirtualAddress(
    const ELFFile &ELF, llvm::ArrayRef<Elf_Shdr> Sections, uint64_t Address) {
  auto ProgramHeaders = ELF.program_headers();
  if (!ProgramHeaders)
    return ProgramHeaders.takeError();
  for (const Elf_Phdr &PH : *ProgramHeaders) {
    uint64_t End = 0;
    if (!checkedAdd(PH.p_vaddr, PH.p_memsz, End))
      return sbfError("legacy program-header address range overflows");
    if (Address < PH.p_vaddr || Address >= End)
      continue;
    uint64_t FileOffset = 0;
    if (!checkedAdd(PH.p_offset, Address - PH.p_vaddr, FileOffset))
      return sbfError("legacy virtual-to-file mapping overflows");
    return FileOffset;
  }
  for (const Elf_Shdr &SH : Sections)
    if (SH.sh_addr == Address)
      return static_cast<uint64_t>(SH.sh_offset);
  return sbfError("legacy dynamic table references an unmapped address");
}

llvm::Expected<std::optional<DynamicSymbolTable>>
findDynamicSymbolTable(llvm::ArrayRef<Elf_Shdr> Sections, uint64_t Address,
                       llvm::ArrayRef<uint8_t> Bytes) {
  using namespace llvm::ELF;
  if (Address == 0)
    return std::nullopt;
  for (size_t Index = 0; Index < Sections.size(); ++Index) {
    const Elf_Shdr &SH = Sections[Index];
    if (SH.sh_addr != Address)
      continue;
    if ((SH.sh_type != SHT_SYMTAB && SH.sh_type != SHT_DYNSYM) ||
        SH.sh_size % sizeof(Elf_Sym) != 0 ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Bytes.size()) ||
        !isOfficialELF64RecordAlignedAt(Bytes, SH.sh_offset))
      return sbfError("invalid legacy dynamic symbol table");
    if (Index > std::numeric_limits<uint32_t>::max())
      return sbfError("legacy dynamic symbol table index overflows");
    return DynamicSymbolTable{&SH, static_cast<uint32_t>(Index)};
  }
  return sbfError("legacy DT_SYMTAB does not identify a symbol section");
}

llvm::Expected<Elf_Sym> readDynamicSymbol(const DynamicSymbolTable &Table,
                                          uint32_t SymbolIndex,
                                          llvm::ArrayRef<uint8_t> Bytes) {
  const size_t Count =
      static_cast<size_t>(Table.Header->sh_size / sizeof(Elf_Sym));
  if (SymbolIndex >= Count)
    return sbfError("legacy relocation symbol index is out of bounds");
  const uint64_t Offset =
      Table.Header->sh_offset + uint64_t{SymbolIndex} * sizeof(Elf_Sym);
  if (!rangeInBounds(Offset, sizeof(Elf_Sym), Bytes.size()))
    return sbfError("legacy dynamic symbol is out of bounds");
  Elf_Sym Symbol;
  std::memcpy(&Symbol, Bytes.data() + Offset, sizeof(Symbol));
  return Symbol;
}

llvm::Expected<llvm::StringRef>
readDynamicSymbolName(const Elf_Sym &Symbol, const Elf_Shdr *StringTable,
                      llvm::ArrayRef<uint8_t> Bytes) {
  using namespace llvm::ELF;
  if (!StringTable || StringTable->sh_type != SHT_STRTAB)
    return sbfError("legacy CALL relocation has no dynamic string table");
  if (Symbol.st_name >= StringTable->sh_size)
    return sbfError("legacy dynamic symbol name offset is out of bounds");

  const uint64_t NameOffset = StringTable->sh_offset + Symbol.st_name;
  const uint64_t Available = StringTable->sh_size - Symbol.st_name;
  const uint64_t BoundedSize =
      std::min<uint64_t>(Available, sbf::kRelocationSymbolNameByteLimit);
  if (!rangeInBounds(NameOffset, BoundedSize, Bytes.size()))
    return sbfError("legacy dynamic symbol name is out of bounds");
  const char *Name = reinterpret_cast<const char *>(Bytes.data() + NameOffset);
  const void *Terminator = std::memchr(Name, '\0', BoundedSize);
  if (!Terminator)
    return sbfError(llvm::Twine("legacy dynamic symbol name exceeds the ") +
                    llvm::Twine(sbf::kRelocationSymbolNameByteLimit) +
                    "-byte relocation limit");
  return llvm::StringRef(Name, static_cast<const char *>(Terminator) - Name);
}

class LegacyRelocationSiteMap {
public:
  explicit LegacyRelocationSiteMap(const BinaryImage &Image) {
    Mappings.reserve(Image.Sections.size());
    for (const Section &Section : Image.Sections)
      if (Section.FileSz != 0)
        Mappings.push_back({Section.FileOff, Section.FileSz, Section.VA});
    std::sort(Mappings.begin(), Mappings.end(),
              [](const Mapping &L, const Mapping &R) {
                return L.FileOffset < R.FileOffset;
              });
  }

  llvm::Expected<va_t> runtimeAddress(uint64_t RawOffset) const {
    auto It = std::upper_bound(Mappings.begin(), Mappings.end(), RawOffset,
                               [](uint64_t Address, const Mapping &Candidate) {
                                 return Address < Candidate.FileOffset;
                               });
    if (It != Mappings.begin()) {
      --It;
      if (RawOffset >= It->FileOffset &&
          RawOffset - It->FileOffset < It->FileSize) {
        uint64_t Address = 0;
        if (!checkedAdd(It->RuntimeAddress, RawOffset - It->FileOffset,
                        Address))
          return sbfError("legacy relocation address overflows");
        return Address;
      }
    }
    const va_t Address = legacyRuntimeAddress(RawOffset);
    if (Address == InvalidVA)
      return sbfError("legacy relocation address overflows");
    return Address;
  }

private:
  struct Mapping {
    uint64_t FileOffset = 0;
    uint64_t FileSize = 0;
    va_t RuntimeAddress = InvalidVA;
  };

  std::vector<Mapping> Mappings;
};

llvm::Error addDynamicRelocations(const ELFFile &ELF,
                                  llvm::ArrayRef<Elf_Shdr> Sections,
                                  const LegacySectionCatalog &Catalog,
                                  BinaryImage &Image) {
  using namespace llvm::ELF;

  auto DynamicTable = findLegacyDynamicTable(ELF, Sections, Image.Raw);
  if (!DynamicTable)
    return DynamicTable.takeError();
  if (!*DynamicTable)
    return llvm::Error::success();
  const LegacyDynamicTags Tags =
      readLegacyDynamicTags(**DynamicTable, Image.Raw);
  std::optional<uint64_t> RelocationFileOffset;
  if (Tags.RelocationAddress != 0) {
    if (Tags.RelocationEntrySize != sizeof(Elf_Rel) ||
        Tags.RelocationSize == 0 || Tags.RelocationSize % sizeof(Elf_Rel) != 0)
      return sbfError("invalid legacy DT_REL table shape");
    auto FileOffset =
        fileOffsetForVirtualAddress(ELF, Sections, Tags.RelocationAddress);
    if (!FileOffset)
      return FileOffset.takeError();
    if (!rangeInBounds(*FileOffset, Tags.RelocationSize, Image.Raw.size()))
      return sbfError("legacy DT_REL table is out of bounds");
    if (!isOfficialELF64RecordAlignedAt(Image.Raw, *FileOffset))
      return sbfError("legacy DT_REL table is misaligned");
    RelocationFileOffset = *FileOffset;
  }

  auto SymbolTable =
      findDynamicSymbolTable(Sections, Tags.SymbolTableAddress, Image.Raw);
  if (!SymbolTable)
    return SymbolTable.takeError();
  if (!RelocationFileOffset)
    return llvm::Error::success();

  std::string RelocationSectionName;
  for (size_t Index = 0; Index < Sections.size(); ++Index) {
    const Elf_Shdr &SH = Sections[Index];
    if (SH.sh_addr == Tags.RelocationAddress) {
      RelocationSectionName = Catalog.Names[Index].str();
      break;
    }
  }

  const size_t Count =
      static_cast<size_t>(Tags.RelocationSize / sizeof(Elf_Rel));
  const LegacyRelocationSiteMap SiteMap(Image);
  for (size_t Ordinal = 0; Ordinal < Count; ++Ordinal) {
    const uint64_t RecordOffset =
        *RelocationFileOffset + Ordinal * sizeof(Elf_Rel);
    Elf_Rel Relocation;
    std::memcpy(&Relocation, Image.Raw.data() + RecordOffset,
                sizeof(Relocation));

    RelocationEntry Entry;
    Entry.Type = Relocation.getType(false);
    Entry.SymbolIndex = Relocation.getSymbol(false);
    Entry.SectionName = RelocationSectionName;
    auto Address = SiteMap.runtimeAddress(Relocation.r_offset);
    if (!Address)
      return Address.takeError();
    Entry.Address = *Address;

    ELFRelocationProvenance Provenance;
    Provenance.Source = (**DynamicTable).Source;
    Provenance.TableVirtualAddress = Tags.RelocationAddress;
    Provenance.TableFileOffset = *RelocationFileOffset;
    Provenance.Ordinal = Ordinal;
    Provenance.RawOffset = Relocation.r_offset;
    Provenance.RawInfo = Relocation.getRInfo(false);

    const sbf::RelocationInfo *Info = sbf::getRelocationInfo(Entry.Type);
    if (Info &&
        Info->SymbolRequirement != sbf::RelocationSymbolRequirement::None) {
      if (!*SymbolTable)
        return sbfError("legacy relocation requires DT_SYMTAB");
      auto RawSymbol =
          readDynamicSymbol(**SymbolTable, Entry.SymbolIndex, Image.Raw);
      if (!RawSymbol)
        return RawSymbol.takeError();

      ELFRelocationSymbol Symbol;
      Symbol.TableSectionIndex = (**SymbolTable).SectionIndex;
      Symbol.NameOffset = RawSymbol->st_name;
      Symbol.SectionIndex = RawSymbol->st_shndx;
      Symbol.Info = RawSymbol->st_info;
      Symbol.Binding = RawSymbol->getBinding();
      Symbol.Type = RawSymbol->getType();
      Symbol.Other = RawSymbol->st_other;
      Symbol.Value = RawSymbol->st_value;
      Symbol.Size = RawSymbol->st_size;

      if (Info->SymbolRequirement ==
          sbf::RelocationSymbolRequirement::RecordAndName) {
        auto Name = readDynamicSymbolName(
            *RawSymbol, Catalog.DynamicSymbolNames, Image.Raw);
        if (!Name)
          return Name.takeError();
        Symbol.Name = Name->str();
        Entry.SymbolName = Name->str();
      }
      Provenance.Symbol = std::move(Symbol);
    }
    Entry.ELF = std::move(Provenance);
    Image.Relocations.push_back(std::move(Entry));
  }
  return llvm::Error::success();
}

void enrichStrictDebugMetadata(const ELFFile &ELF, const Elf_Ehdr &Header,
                               sbf::Metadata &Metadata, BinaryImage &Image) {
  using namespace llvm::ELF;
  if (Header.e_shoff == 0 && Header.e_shnum == 0 &&
      Header.e_shstrndx == SHN_UNDEF) {
    Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Unavailable;
    return;
  }

  auto Sections = ELF.sections();
  if (!Sections) {
    llvm::consumeError(Sections.takeError());
    Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Malformed;
    return;
  }
  if (Sections->empty()) {
    Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Unavailable;
    return;
  }

  llvm::StringRef SectionNames;
  if (Header.e_shstrndx != SHN_UNDEF) {
    auto Names = ELF.getSectionStringTable(*Sections);
    if (!Names) {
      llvm::consumeError(Names.takeError());
      Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Malformed;
      return;
    }
    SectionNames = *Names;
  }

  Metadata.DebugEnrichment =
      addStrictDebugSymbols(*Sections, SectionNames, Image.Raw, Image);
}

llvm::Error loadStrict(const ELFFile &ELF, const Elf_Ehdr &Header,
                       BinaryImage &Image, sbf::Metadata &Metadata) {
  using namespace llvm::ELF;
  if (Header.e_machine != sbf::kELFMachineBPF)
    return sbfError(llvm::Twine("v3+ strict ELF requires ") +
                    sbf::kELFMachineBPFName);
  if (Header.e_phoff != sizeof(Elf_Ehdr) ||
      Header.e_ehsize != sizeof(Elf_Ehdr) ||
      Header.e_phentsize != sizeof(Elf_Phdr) || Header.e_phnum == 0)
    return sbfError("invalid strict ELF header layout");

  auto Headers = ELF.program_headers();
  if (!Headers)
    return Headers.takeError();
  if (Headers->empty())
    return sbfError("strict ELF has no program headers");

  const bool HasRodata = (*Headers)[0].p_flags == PF_R;
  const size_t RequiredHeaders = HasRodata ? 2 : 1;
  if (Headers->size() < RequiredHeaders)
    return sbfError("strict ELF is missing its bytecode program header");

  uint64_t ExpectedOffset =
      sizeof(Elf_Ehdr) + Header.e_phnum * sizeof(Elf_Phdr);
  const Elf_Phdr *RodataHeader = HasRodata ? &(*Headers)[0] : nullptr;
  const Elf_Phdr &TextHeader = (*Headers)[HasRodata ? 1 : 0];

  auto ValidateHeader = [&](const Elf_Phdr &PH, uint32_t Flags,
                            uint64_t Address) -> llvm::Error {
    if (PH.p_type != PT_LOAD || PH.p_flags != Flags ||
        PH.p_offset != ExpectedOffset || PH.p_offset >= Image.Raw.size() ||
        PH.p_offset % sbf::kInstructionSize != 0 || PH.p_vaddr != Address ||
        PH.p_paddr != Address || PH.p_filesz != PH.p_memsz ||
        !rangeInBounds(PH.p_offset, PH.p_filesz, Image.Raw.size()) ||
        PH.p_filesz % sbf::kInstructionSize != 0 ||
        PH.p_memsz >= sbf::kMemoryRegionSize)
      return sbfError("invalid strict ELF program header");
    ExpectedOffset += PH.p_filesz;
    return llvm::Error::success();
  };

  if (RodataHeader)
    if (llvm::Error Error =
            ValidateHeader(*RodataHeader, PF_R, sbf::kRodataStartV3))
      return Error;
  if (llvm::Error Error = ValidateHeader(TextHeader, PF_X, sbf::kBytecodeStart))
    return Error;
  if (TextHeader.p_filesz == 0)
    return sbfError("strict ELF bytecode segment is empty");
  if (Header.e_entry % sbf::kInstructionSize != 0 ||
      Header.e_entry < TextHeader.p_vaddr ||
      Header.e_entry - TextHeader.p_vaddr >= TextHeader.p_memsz)
    return sbfError("strict ELF entry point is outside bytecode");

  if (RodataHeader) {
    addMappedRegion(Image, sbf::kRodataSectionName, RodataHeader->p_vaddr,
                    RodataHeader->p_offset, RodataHeader->p_filesz,
                    RodataHeader->p_memsz, SegmentFlags::Readable,
                    sbf::kInstructionSize);
    Metadata.RodataFile = {RodataHeader->p_offset, RodataHeader->p_filesz};
    Metadata.RodataVM = {RodataHeader->p_vaddr, RodataHeader->p_memsz};
  }
  addMappedRegion(Image, sbf::kTextSectionName, TextHeader.p_vaddr,
                  TextHeader.p_offset, TextHeader.p_filesz, TextHeader.p_memsz,
                  SegmentFlags::Executable, sbf::kInstructionSize);
  Metadata.TextFile = {TextHeader.p_offset, TextHeader.p_filesz};
  Metadata.TextVM = {TextHeader.p_vaddr, TextHeader.p_memsz};

  enrichStrictDebugMetadata(ELF, Header, Metadata, Image);
  return llvm::Error::success();
}

llvm::Error loadLegacy(const ELFFile &ELF, const Elf_Ehdr &Header,
                       BinaryImage &Image, sbf::Metadata &Metadata) {
  using namespace llvm::ELF;
  if (Header.e_machine != sbf::kELFMachineBPF &&
      Header.e_machine != sbf::kELFMachineSBPF)
    return sbfError("legacy ELF has an unsupported machine");
  if (Header.e_type != ET_DYN)
    return sbfError("legacy SBF ELF type must be ET_DYN");

  auto Sections = ELF.sections();
  if (!Sections)
    return Sections.takeError();
  if (Sections->empty())
    return sbfError("legacy SBF ELF has no section table");
  if (llvm::Error Error =
          validateLegacyParserLayout(ELF, Header, *Sections, Image.Raw))
    return Error;
  auto Names = ELF.getSectionStringTable(*Sections);
  if (!Names)
    return Names.takeError();
  const llvm::StringRef SectionNames = *Names;
  auto Catalog = parseLegacySectionCatalog(*Sections, SectionNames);
  if (!Catalog)
    return Catalog.takeError();

  const Elf_Shdr *Text = nullptr;
  va_t TextVA = InvalidVA;
  for (size_t Index = 0; Index < Sections->size(); ++Index) {
    const Elf_Shdr &SH = (*Sections)[Index];
    const llvm::StringRef Name = Catalog->Names[Index];
    const bool IsRuntimeSection = sbf::isLegacyReadOnlySectionName(Name);
    if (IsRuntimeSection) {
      sbf::LegacyReadOnlySectionLayout Layout;
      Layout.SectionIndex = static_cast<uint32_t>(Index);
      Layout.RawAddress = SH.sh_addr;
      Layout.FileOffset = SH.sh_offset;
      Layout.FileSize = SH.sh_type == SHT_NOBITS ? 0 : SH.sh_size;
      Metadata.LegacyReadOnlySections.push_back(Layout);
    }
    if (Name == sbf::kTextSectionName) {
      if (Text)
        return sbfError("legacy ELF contains multiple .text sections");
      Text = &SH;
    }
    if (Name.starts_with(sbf::kBSSSectionPrefix) ||
        ((SH.sh_flags & SHF_WRITE) != 0 &&
         Name.starts_with(sbf::kDataSectionPrefix) &&
         !Name.starts_with(sbf::kDataRelSectionPrefix)))
      return sbfError(llvm::Twine("writable legacy section is unsupported: ") +
                      Name);
    if (!rangeInBounds(SH.sh_offset, SH.sh_size, Image.Raw.size()))
      return sbfError("legacy section is out of file bounds");
    // The lenient sbpf loader's runtime inventory is name-driven.  SHF_ALLOC
    // is deliberately irrelevant here, and unknown sections remain parser
    // input only: they are range-checked above but never mapped into the VM.
    if (!IsRuntimeSection || SH.sh_size == 0)
      continue;

    va_t Address = InvalidVA;
    if (Name == sbf::kTextSectionName) {
      auto RuntimeAddress = legacyTextRuntimeAddress(SH.sh_addr, SH.sh_size);
      if (!RuntimeAddress)
        return RuntimeAddress.takeError();
      Address = *RuntimeAddress;
      TextVA = Address;
    } else {
      Address = legacyRuntimeAddress(SH.sh_addr);
    }
    if (Address == InvalidVA)
      return sbfError("legacy section virtual address overflows");
    SegmentFlags Flags = elfSHFlagsToNd(SH.sh_flags);
    const uint64_t FileSize = SH.sh_type == SHT_NOBITS ? 0 : SH.sh_size;
    const uint32_t Alignment =
        SH.sh_addralign <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(SH.sh_addralign)
            : 0;
    addMappedRegion(Image, Name, Address, SH.sh_offset, FileSize, SH.sh_size,
                    Flags, Alignment);
    if (Name == sbf::kRodataSectionName && Metadata.RodataFile.Size == 0) {
      Metadata.RodataFile = {SH.sh_offset, SH.sh_size};
      Metadata.RodataVM = {Address, SH.sh_size};
    }
  }
  if (!Text)
    return sbfError("legacy ELF must contain one .text section");

  const va_t Entry = legacyRuntimeAddress(Header.e_entry);
  if (TextVA == InvalidVA || Entry == InvalidVA ||
      Header.e_entry < Text->sh_addr ||
      Header.e_entry - Text->sh_addr >= Text->sh_size)
    return sbfError("legacy ELF entry point is outside .text");
  const uint64_t TextFileSize = Text->sh_type == SHT_NOBITS ? 0 : Text->sh_size;
  Metadata.TextFile = {Text->sh_offset, TextFileSize};
  Metadata.TextVM = {TextVA, Text->sh_size};
  Image.Entry = Entry;

  Metadata.DebugEnrichment =
      addLegacyDebugSymbols(*Catalog, Image.Raw, Metadata.Version, Image);
  // Official lenient loading relocates before it validates entry alignment.
  // ProgramImage applies the relocations and performs that final alignment
  // check in the same order; the loader only enforces validate()'s bounds.
  return addDynamicRelocations(ELF, *Sections, *Catalog, Image);
}

} // namespace

llvm::Expected<bool> loadSBFELF(BinaryImage &Image) {
  if (!hasSBFMachine(Image.Raw))
    return false;

  auto Parsed = parseELF(Image);
  if (!Parsed)
    return Parsed.takeError();
  const ELFFile &ELF = *Parsed;
  const Elf_Ehdr &Header = ELF.getHeader();
  const sbf::Version Version = sbf::versionFromELFFlags(Header.e_flags);
  if (!sbf::isConcreteVersion(Version))
    return sbfError(llvm::Twine("unsupported SBF version in ELF flags: ") +
                    llvm::Twine(static_cast<uint32_t>(Header.e_flags)));
  const bool Strict =
      sbf::versionHasFeature(Version, sbf::VersionFeature::StrictELF);
  if (llvm::Error Error = validateIdentity(Header, Strict))
    return std::move(Error);

  Image.Arch = Arch::SBF;
  Image.Bits = Bitness::Bits64;
  Image.Mode = InstructionMode::Default;
  Image.IsRelocatable = false;
  Image.Entry = Header.e_entry;

  sbf::Metadata Metadata;
  Metadata.Machine = Header.e_machine;
  Metadata.ELFFlags = Header.e_flags;
  Metadata.Version = Version;
  Metadata.StrictLayout = Strict;
  if (llvm::Error Error = Strict ? loadStrict(ELF, Header, Image, Metadata)
                                 : loadLegacy(ELF, Header, Image, Metadata))
    return std::move(Error);

  if (Image.Segments.empty())
    return sbfError("ELF contains no mapped SBF regions");
  Image.Base = Image.Segments.front().VA;
  for (const Segment &Segment : Image.Segments)
    Image.Base = std::min(Image.Base, Segment.VA);

  bool HasEntry = false;
  for (const Symbol &Symbol : Image.Symbols)
    HasEntry |= Symbol.Addr == Image.Entry && Symbol.IsFunc;
  if (!HasEntry) {
    Symbol Entry;
    Entry.Name = sbf::kEntrySymbolName.str();
    Entry.Addr = Image.Entry;
    Entry.IsFunc = true;
    Image.Symbols.push_back(std::move(Entry));
  }
  Image.SBF = Metadata;
  return true;
}

} // namespace neverd
