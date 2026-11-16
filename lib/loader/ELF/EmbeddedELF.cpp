//===- EmbeddedELF.cpp - Embedded ELF relocation evidence ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reads contained objects without installing their sections in the parent's
/// address space. Only relocation forms with an exact file-backed S+A target
/// become references; other records remain explicitly unqualified.
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/EmbeddedELF.h"

#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace neverd {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

/// The field extent for forms whose symbolic target is S+A. PC-relative x86
/// fields need instruction-specific next-PC evidence, so are not guessed from
/// the field width. TLS/GOT/size relocations likewise do not name ordinary
/// data.
unsigned referenceWidth(uint16_t Machine, uint32_t Type, bool ExplicitAddend) {
  using namespace llvm::ELF;
  switch (Machine) {
  case EM_AARCH64:
    if (!ExplicitAddend)
      return 0;
    switch (Type) {
    case R_AARCH64_ABS64:
    case R_AARCH64_PREL64:
      return 8;
    case R_AARCH64_ABS16:
    case R_AARCH64_PREL16:
      return 2;
    case R_AARCH64_ABS32:
    case R_AARCH64_PREL32:
    case R_AARCH64_ADR_PREL_LO21:
    case R_AARCH64_ADR_PREL_PG_HI21:
    case R_AARCH64_ADR_PREL_PG_HI21_NC:
    case R_AARCH64_ADD_ABS_LO12_NC:
    case R_AARCH64_LDST8_ABS_LO12_NC:
    case R_AARCH64_LDST16_ABS_LO12_NC:
    case R_AARCH64_LDST32_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:
    case R_AARCH64_LDST128_ABS_LO12_NC:
    case R_AARCH64_LD_PREL_LO19:
    case R_AARCH64_TSTBR14:
    case R_AARCH64_CONDBR19:
    case R_AARCH64_JUMP26:
    case R_AARCH64_CALL26:
      return 4;
    default:
      return 0;
    }
  case EM_X86_64:
    switch (Type) {
    case R_X86_64_64:
      return 8;
    case R_X86_64_32:
    case R_X86_64_32S:
      return 4;
    case R_X86_64_16:
      return 2;
    case R_X86_64_8:
      return 1;
    default:
      return 0;
    }
  case EM_386:
    return Type == R_386_32   ? 4
           : Type == R_386_16 ? 2
           : Type == R_386_8  ? 1
                              : 0;
  case EM_ARM:
    return Type == R_ARM_ABS32   ? 4
           : Type == R_ARM_ABS16 ? 2
           : Type == R_ARM_ABS8  ? 1
                                 : 0;
  default:
    return 0;
  }
}

template <typename ELFT>
llvm::Error readObject(llvm::StringRef Bytes, EmbeddedELFObject &Out) {
  using namespace llvm::ELF;
  auto File = llvm::object::ELFFile<ELFT>::create(Bytes);
  if (!File)
    return File.takeError();
  const auto &Header = File->getHeader();
  Out.Machine = Header.e_machine;
  Out.Type = Header.e_type;
  if (Header.e_version != EV_CURRENT || Header.e_ehsize != sizeof(Header))
    return invalid("invalid ELF header version or size");
  auto Sections = File->sections();
  if (!Sections)
    return Sections.takeError();
  auto Programs = File->program_headers();
  if (!Programs)
    return Programs.takeError();
  auto Names = File->getSectionStringTable(*Sections);
  if (!Names)
    return Names.takeError();
  Out.Size = sizeof(Header);
  const auto extent = [&](uint64_t Offset, uint64_t Size) -> llvm::Error {
    if (Offset > Bytes.size() || Size > Bytes.size() - Offset)
      return invalid("embedded ELF range exceeds containing file");
    Out.Size = std::max(Out.Size, Offset + Size);
    return llvm::Error::success();
  };
  if (!Sections->empty())
    if (auto E = extent(Header.e_shoff,
                        Sections->size() * sizeof(typename ELFT::Shdr)))
      return E;
  if (!Programs->empty())
    if (auto E = extent(Header.e_phoff,
                        Programs->size() * sizeof(typename ELFT::Phdr)))
      return E;
  for (const auto &Program : *Programs)
    if (auto E = extent(Program.p_offset, Program.p_filesz))
      return E;
  for (const auto &Section : *Sections) {
    auto Name = File->getSectionName(Section, *Names);
    if (!Name)
      return Name.takeError();
    if (Section.sh_type != SHT_NOBITS)
      if (auto E = extent(Section.sh_offset, Section.sh_size))
        return E;
    Out.Sections.push_back({Name->str(), Section.sh_offset, Section.sh_size});
  }

  struct Symbols {
    llvm::ArrayRef<typename ELFT::Sym> Rows;
    llvm::StringRef Names;
  };
  llvm::DenseMap<uint32_t, Symbols> SymbolTables;
  for (const auto &Section : *Sections) {
    const bool ExplicitAddend = Section.sh_type == SHT_RELA;
    if (!ExplicitAddend && Section.sh_type != SHT_REL)
      continue;
    if (Section.sh_flags & SHF_COMPRESSED)
      return invalid(
          "compressed embedded ELF relocation tables are unsupported");
    const unsigned EntrySize = ExplicitAddend ? sizeof(typename ELFT::Rela)
                                              : sizeof(typename ELFT::Rel);
    if (Section.sh_entsize != EntrySize || Section.sh_size % EntrySize)
      return invalid("invalid embedded ELF relocation entry size");
    const uint64_t Count = Section.sh_size / EntrySize;
    Out.Relocations += Count;
    // Linked ELF relocation tables use VAs and can target synthesized GOT/TLS
    // state. Inventory them, without pretending their offsets are ET_REL data.
    if (Out.Type != ET_REL) {
      Out.UnsupportedRelocations += Count;
      continue;
    }
    if (!Section.sh_info || Section.sh_info >= Sections->size() ||
        Section.sh_link >= Sections->size())
      return invalid("invalid embedded ELF relocation section index");
    const auto &Source = (*Sections)[Section.sh_info];
    const auto &SymbolSection = (*Sections)[Section.sh_link];
    if (SymbolSection.sh_type != SHT_SYMTAB &&
        SymbolSection.sh_type != SHT_DYNSYM)
      return invalid("embedded ELF relocation has no symbol table");
    if (SymbolSection.sh_flags & SHF_COMPRESSED)
      return invalid("compressed embedded ELF symbol tables are unsupported");
    auto It = SymbolTables.find(Section.sh_link);
    if (It == SymbolTables.end()) {
      auto Rows = File->symbols(&SymbolSection);
      if (!Rows)
        return Rows.takeError();
      auto Strings = File->getStringTableForSymtab(SymbolSection, *Sections);
      if (!Strings)
        return Strings.takeError();
      It = SymbolTables.try_emplace(Section.sh_link, Symbols{*Rows, *Strings})
               .first;
    }
    const auto &Symbols = It->second;
    const auto record = [&](uint64_t Offset, uint32_t Type,
                            uint32_t SymbolIndex, int64_t Addend,
                            uint64_t Ordinal) -> llvm::Error {
      if (SymbolIndex >= Symbols.Rows.size() || Offset >= Source.sh_size)
        return invalid(
            "embedded ELF relocation source or symbol is out of bounds");
      const auto &Symbol = Symbols.Rows[SymbolIndex];
      auto Name = Symbol.getName(Symbols.Names);
      if (!Name)
        return Name.takeError();
      const unsigned Width = referenceWidth(Out.Machine, Type, ExplicitAddend);
      if (!Width) {
        ++Out.UnsupportedRelocations;
        return llvm::Error::success();
      }
      if (Width > Source.sh_size - Offset)
        return invalid("embedded ELF relocation field crosses its section");
      if (Source.sh_type == SHT_NOBITS || !(Source.sh_flags & SHF_ALLOC) ||
          Symbol.st_shndx == SHN_UNDEF || Symbol.st_shndx >= SHN_LORESERVE) {
        ++Out.UnresolvedRelocations;
        return llvm::Error::success();
      }
      if (Symbol.st_shndx >= Sections->size())
        return invalid("embedded ELF symbol section is out of bounds");
      const auto &Target = (*Sections)[Symbol.st_shndx];
      if ((Source.sh_flags | Target.sh_flags) & SHF_COMPRESSED) {
        ++Out.UnsupportedRelocations;
        return llvm::Error::success();
      }
      if (Target.sh_type == SHT_NOBITS || !(Target.sh_flags & SHF_ALLOC)) {
        ++Out.UnresolvedRelocations;
        return llvm::Error::success();
      }
      if (!ExplicitAddend) {
        const auto *Field = reinterpret_cast<const uint8_t *>(Bytes.data()) +
                            Source.sh_offset + Offset;
        uint64_t Raw = 0;
        for (unsigned I = 0; I < Width; ++I)
          Raw |= uint64_t(Field[I])
                 << ((Out.BigEndian ? Width - 1 - I : I) * 8);
        Addend = llvm::SignExtend64(Raw, Width * 8);
      }
      uint64_t TargetOffset = Symbol.st_value;
      if (Addend < 0) {
        const uint64_t Magnitude = uint64_t(-(Addend + 1)) + 1;
        if (Magnitude > TargetOffset) {
          ++Out.UnresolvedRelocations;
          return llvm::Error::success();
        }
        TargetOffset -= Magnitude;
      } else {
        if (uint64_t(Addend) >
            std::numeric_limits<uint64_t>::max() - TargetOffset) {
          ++Out.UnresolvedRelocations;
          return llvm::Error::success();
        }
        TargetOffset += Addend;
      }
      if (Out.Machine == EM_ARM && Symbol.getType() == STT_FUNC)
        TargetOffset &= ~uint64_t(1);
      if (TargetOffset >= Target.sh_size) {
        ++Out.UnresolvedRelocations;
        return llvm::Error::success();
      }
      Out.References.push_back(
          {Source.sh_offset + Offset, Target.sh_offset + TargetOffset,
           Section.sh_offset + Ordinal * EntrySize, Section.sh_info,
           Symbol.st_shndx, Type, Addend, Name->str()});
      return llvm::Error::success();
    };
    uint64_t Ordinal = 0;
    if (ExplicitAddend) {
      auto Rows = File->relas(Section);
      if (!Rows)
        return Rows.takeError();
      for (const auto &R : *Rows)
        if (auto E =
                record(R.r_offset, R.getType(File->isMips64EL()),
                       R.getSymbol(File->isMips64EL()), R.r_addend, Ordinal++))
          return E;
    } else {
      auto Rows = File->rels(Section);
      if (!Rows)
        return Rows.takeError();
      for (const auto &R : *Rows)
        if (auto E = record(R.r_offset, R.getType(File->isMips64EL()),
                            R.getSymbol(File->isMips64EL()), 0, Ordinal++))
          return E;
    }
  }
  return llvm::Error::success();
}
} // namespace

std::vector<EmbeddedELFObject> scanEmbeddedELF(llvm::ArrayRef<uint8_t> Bytes) {
  using namespace llvm::ELF;
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  std::vector<EmbeddedELFObject> Objects;
  for (size_t At = Data.find(llvm::StringRef(ElfMagic, 4), 1);
       At != llvm::StringRef::npos;
       At = Data.find(llvm::StringRef(ElfMagic, 4), At + 1)) {
    if (Data.size() - At < EI_NIDENT || Bytes[At + EI_VERSION] != EV_CURRENT)
      continue;
    const unsigned Class = Bytes[At + EI_CLASS], Encoding = Bytes[At + EI_DATA];
    if ((Class != ELFCLASS32 && Class != ELFCLASS64) ||
        (Encoding != ELFDATA2LSB && Encoding != ELFDATA2MSB))
      continue;
    EmbeddedELFObject Object;
    Object.Offset = At;
    Object.Bits = Class == ELFCLASS64 ? 64 : 32;
    Object.BigEndian = Encoding == ELFDATA2MSB;
    Objects.push_back(std::move(Object));
  }
  parallelForEach(Objects.size(), [&](auto &Claim, size_t Total) {
    for (size_t I = Claim(); I < Total; I = Claim()) {
      auto &Object = Objects[I];
      const auto Payload = Data.drop_front(Object.Offset);
      llvm::Error E =
          Object.Bits == 64
              ? (Object.BigEndian
                     ? readObject<llvm::object::ELF64BE>(Payload, Object)
                     : readObject<llvm::object::ELF64LE>(Payload, Object))
              : (Object.BigEndian
                     ? readObject<llvm::object::ELF32BE>(Payload, Object)
                     : readObject<llvm::object::ELF32LE>(Payload, Object));
      if (E) {
        Object.Diagnostic = llvm::toString(std::move(E));
        Object.References.clear();
      }
    }
  });
  return Objects;
}
} // namespace neverd
