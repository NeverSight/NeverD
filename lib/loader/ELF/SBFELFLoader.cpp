//===- SBFELFLoader.cpp - Solana SBF ELF loader -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/SBFELFLoader.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/Version.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

namespace neverd {
namespace {

using ELFT = llvm::object::ELF64LE;
using ELFFile = llvm::object::ELFFile<ELFT>;
using Elf_Ehdr = ELFT::Ehdr;
using Elf_Phdr = ELFT::Phdr;
using Elf_Shdr = ELFT::Shdr;
using Elf_Sym = ELFT::Sym;
using Elf_Rel = ELFT::Rel;
using Elf_Rela = ELFT::Rela;

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

va_t legacyRuntimeAddress(uint64_t Address) {
  if (Address >= sbf::kBytecodeStart)
    return Address;
  uint64_t Result = 0;
  if (!checkedAdd(sbf::kBytecodeStart, Address, Result))
    return InvalidVA;
  return Result;
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

llvm::Error addSymbols(const ELFFile &ELF, llvm::ArrayRef<Elf_Shdr> Sections,
                       llvm::StringRef SectionNames, sbf::Version Version,
                       BinaryImage &Image) {
  using namespace llvm::ELF;
  for (const Elf_Shdr &SH : Sections) {
    if (SH.sh_type != SHT_SYMTAB && SH.sh_type != SHT_DYNSYM)
      continue;
    auto Symbols = ELF.symbols(&SH);
    if (!Symbols)
      return Symbols.takeError();
    auto StringTable = ELF.getStringTableForSymtab(SH);
    if (!StringTable)
      return StringTable.takeError();

    for (const Elf_Sym &RawSymbol : *Symbols) {
      if (RawSymbol.st_name == 0)
        continue;
      auto Name = RawSymbol.getName(*StringTable);
      if (!Name)
        return Name.takeError();
      if (Name->empty())
        continue;

      va_t Address = RawSymbol.st_value;
      if (!sbf::versionHasFeature(Version, sbf::VersionFeature::StrictELF) &&
          RawSymbol.st_shndx != SHN_UNDEF) {
        Address = legacyRuntimeAddress(Address);
        if (Address == InvalidVA)
          return sbfError("symbol virtual address overflows");
      }

      Symbol Symbol;
      Symbol.Name = Name->str();
      Symbol.Addr = Address;
      Symbol.Size = RawSymbol.st_size;
      Symbol.IsFunc = RawSymbol.getType() == STT_FUNC;
      Image.Symbols.push_back(std::move(Symbol));
    }
  }
  (void)SectionNames;
  return llvm::Error::success();
}

llvm::Error addRelocations(const ELFFile &ELF,
                           llvm::ArrayRef<Elf_Shdr> Sections,
                           llvm::StringRef SectionNames, BinaryImage &Image) {
  using namespace llvm::ELF;
  auto SectionName = [&](const Elf_Shdr &SH) -> llvm::StringRef {
    if (SH.sh_name >= SectionNames.size())
      return {};
    return SectionNames.drop_front(SH.sh_name).split('\0').first;
  };

  for (const Elf_Shdr &SH : Sections) {
    const bool IsRela = SH.sh_type == SHT_RELA;
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    const uint64_t ExpectedEntrySize =
        IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    if (SH.sh_entsize != ExpectedEntrySize ||
        SH.sh_size % ExpectedEntrySize != 0)
      return sbfError("relocation section has an invalid entry size");
    if (!rangeInBounds(SH.sh_offset, SH.sh_size, Image.Raw.size()))
      return sbfError("relocation section is out of bounds");
    if (SH.sh_info >= Sections.size())
      return sbfError("relocation target section index is out of bounds");

    const Elf_Shdr &Target = Sections[SH.sh_info];
    const va_t TargetVA = legacyRuntimeAddress(Target.sh_addr);
    if (TargetVA == InvalidVA)
      return sbfError("relocation target address overflows");

    const Elf_Shdr *SymbolSection =
        SH.sh_link < Sections.size() ? &Sections[SH.sh_link] : nullptr;
    llvm::StringRef SymbolNames;
    if (SymbolSection) {
      auto Names = ELF.getStringTableForSymtab(*SymbolSection);
      if (!Names)
        return Names.takeError();
      SymbolNames = *Names;
    }

    const size_t Count = static_cast<size_t>(SH.sh_size / ExpectedEntrySize);
    for (size_t I = 0; I < Count; ++I) {
      const uint64_t Offset = SH.sh_offset + I * SH.sh_entsize;
      RelocationEntry Entry;
      uint32_t SymbolIndex = 0;
      uint64_t RawAddress = 0;
      if (IsRela) {
        if (!rangeInBounds(Offset, sizeof(Elf_Rela), Image.Raw.size()))
          return sbfError("RELA entry is out of bounds");
        Elf_Rela Relocation;
        std::memcpy(&Relocation, Image.Raw.data() + Offset, sizeof(Relocation));
        RawAddress = Relocation.r_offset;
        Entry.Addend = Relocation.r_addend;
        Entry.HasExplicitAddend = true;
        Entry.Type = Relocation.getType(false);
        SymbolIndex = Relocation.getSymbol(false);
      } else {
        if (!rangeInBounds(Offset, sizeof(Elf_Rel), Image.Raw.size()))
          return sbfError("REL entry is out of bounds");
        Elf_Rel Relocation;
        std::memcpy(&Relocation, Image.Raw.data() + Offset, sizeof(Relocation));
        RawAddress = Relocation.r_offset;
        Entry.Type = Relocation.getType(false);
        SymbolIndex = Relocation.getSymbol(false);
      }

      if (RawAddress < Target.sh_size)
        Entry.Address = TargetVA + RawAddress;
      else
        Entry.Address = legacyRuntimeAddress(RawAddress);
      if (Entry.Address == InvalidVA)
        return sbfError("relocation address overflows");
      Entry.SymbolIndex = SymbolIndex;
      Entry.SectionName = SectionName(SH).str();

      if (SymbolSection && SymbolIndex != 0) {
        auto Symbols = ELF.symbols(SymbolSection);
        if (!Symbols)
          return Symbols.takeError();
        if (SymbolIndex >= Symbols->size())
          return sbfError("relocation symbol index is out of bounds");
        auto Name = (*Symbols)[SymbolIndex].getName(SymbolNames);
        if (!Name)
          return Name.takeError();
        Entry.SymbolName = Name->str();
      }
      Image.Relocations.push_back(std::move(Entry));
    }
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

  const size_t OriginalSymbolCount = Image.Symbols.size();
  if (llvm::Error Error =
          addSymbols(ELF, *Sections, SectionNames, Metadata.Version, Image)) {
    llvm::consumeError(std::move(Error));
    Image.Symbols.resize(OriginalSymbolCount);
    Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Malformed;
    return;
  }
  Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Complete;
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
  auto Names = ELF.getSectionStringTable(*Sections);
  if (!Names)
    return Names.takeError();
  const llvm::StringRef SectionNames = *Names;
  auto SectionName = [&](const Elf_Shdr &SH) -> llvm::StringRef {
    if (SH.sh_name >= SectionNames.size())
      return {};
    return SectionNames.drop_front(SH.sh_name).split('\0').first;
  };

  const Elf_Shdr *Text = nullptr;
  for (const Elf_Shdr &SH : *Sections) {
    const llvm::StringRef Name = SectionName(SH);
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
    if ((SH.sh_flags & SHF_ALLOC) == 0 || SH.sh_size == 0)
      continue;
    if (Name.empty())
      continue;
    if (SH.sh_type == SHT_NOBITS)
      return sbfError("writable/NOBITS legacy sections are unsupported");
    if (SH.sh_addr > sbf::kBytecodeStart)
      return sbfError("legacy section virtual address exceeds bytecode range");
    const uint64_t Alignment = SH.sh_addralign == 0 ? 1 : SH.sh_addralign;
    if (Alignment > std::numeric_limits<uint32_t>::max() ||
        (Alignment & (Alignment - 1)) != 0)
      return sbfError("legacy section alignment is invalid");

    const va_t Address = legacyRuntimeAddress(SH.sh_addr);
    if (Address == InvalidVA)
      return sbfError("legacy section virtual address overflows");
    SegmentFlags Flags = elfSHFlagsToNd(SH.sh_flags);
    addMappedRegion(Image, Name, Address, SH.sh_offset, SH.sh_size, SH.sh_size,
                    Flags, static_cast<uint32_t>(Alignment));
    if (Name == sbf::kRodataSectionName && Metadata.RodataFile.Size == 0) {
      Metadata.RodataFile = {SH.sh_offset, SH.sh_size};
      Metadata.RodataVM = {Address, SH.sh_size};
    }
  }
  if (!Text || Text->sh_size == 0)
    return sbfError("legacy ELF must contain one non-empty .text section");
  if (Text->sh_size % sbf::kInstructionSize != 0)
    return sbfError("legacy .text size is not instruction-aligned");

  const va_t TextVA = legacyRuntimeAddress(Text->sh_addr);
  const va_t Entry = legacyRuntimeAddress(Header.e_entry);
  if (TextVA == InvalidVA || Entry == InvalidVA ||
      Header.e_entry < Text->sh_addr ||
      Header.e_entry - Text->sh_addr >= Text->sh_size ||
      (Header.e_entry - Text->sh_addr) % sbf::kInstructionSize != 0)
    return sbfError("legacy ELF entry point is outside .text");
  Metadata.TextFile = {Text->sh_offset, Text->sh_size};
  Metadata.TextVM = {TextVA, Text->sh_size};
  Image.Entry = Entry;

  if (llvm::Error Error =
          addSymbols(ELF, *Sections, SectionNames, Metadata.Version, Image))
    return Error;
  Metadata.DebugEnrichment = sbf::DebugEnrichmentStatus::Complete;
  return addRelocations(ELF, *Sections, SectionNames, Image);
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
