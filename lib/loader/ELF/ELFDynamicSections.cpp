//===- ELFDynamicSections.cpp - Tables of an ELF without sections ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A linked ELF image keeps running without its section header table
/// (`sstrip`, `llvm-objcopy --strip-sections`): the dynamic linker finds every
/// table it reads through PT_DYNAMIC, and the unwinder finds .eh_frame_hdr
/// through PT_GNU_EH_FRAME.  The headers built here describe those same
/// tables, each from the record the runtime itself reads, so the loader's
/// parsers read them as they read a complete file's.  Nothing there bounds
/// .plt, .got or .text, and none is made.
///
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"

#include "neverd/object/SectionNames.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>

namespace neverd {
namespace elf_loader {
namespace detail {

template <typename ELFT>
DynamicSections<ELFT>
reconstructDynamicSections(const llvm::object::ELFFile<ELFT> &ELF,
                           BinaryImage &Img) {
  using namespace llvm::ELF;
  namespace names = section_names::elf;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Phdr = typename ELFT::Phdr;
  using Elf_Dyn = typename ELFT::Dyn;

  DynamicSections<ELFT> Out;
  Out.Names.push_back('\0');
  // Index 0 is SHN_UNDEF in every section header table.
  Out.Headers.emplace_back();

  auto HeadersOr = ELF.program_headers();
  if (!HeadersOr) {
    Img.addLoadDiagnostic("elf.program_headers_invalid",
                          llvm::toString(HeadersOr.takeError()));
    return Out;
  }
  const uint64_t FileSize = ELF.getBufSize();
  // A table the runtime would read wrongly or not at all is reported and
  // left out; the rest of the image still loads.
  auto Skip = [&](llvm::StringRef Name, const llvm::Twine &Why) {
    Img.addLoadDiagnostic(
        "elf.dynamic_table_invalid",
        llvm::formatv("{0}: {1}; it was not read", Name, Why.str()).str());
  };

  // The file offset of the \p Size bytes at \p VA: where the one PT_LOAD
  // whose file bytes hold all of them puts them.
  auto FileOffset = [&](llvm::StringRef Name, uint64_t VA,
                        uint64_t Size) -> std::optional<uint64_t> {
    std::optional<uint64_t> Offset;
    for (const Elf_Phdr &P : *HeadersOr) {
      if (P.p_type != PT_LOAD || VA < P.p_vaddr ||
          VA - P.p_vaddr > P.p_filesz || Size > P.p_filesz - (VA - P.p_vaddr))
        continue;
      const uint64_t Candidate = P.p_offset + (VA - P.p_vaddr);
      if (Offset && *Offset != Candidate) {
        Skip(Name, "two loaded segments map it to different file bytes");
        return std::nullopt;
      }
      Offset = Candidate;
    }
    if (!Offset || *Offset > FileSize || Size > FileSize - *Offset) {
      Skip(Name, llvm::formatv("{0:x}+{1:x} is not in the file bytes of a "
                               "loaded segment",
                               VA, Size));
      return std::nullopt;
    }
    return Offset;
  };
  auto Add = [&](llvm::StringRef Name, uint32_t Type, uint64_t VA,
                 uint64_t Offset, uint64_t Size, uint64_t EntrySize,
                 uint32_t Link) {
    Elf_Shdr SH{};
    SH.sh_name = static_cast<uint32_t>(Out.Names.size());
    Out.Names.append(Name.begin(), Name.end());
    Out.Names.push_back('\0');
    SH.sh_type = Type;
    SH.sh_flags = SHF_ALLOC;
    SH.sh_addr = VA;
    SH.sh_offset = Offset;
    SH.sh_size = Size;
    SH.sh_link = Link;
    SH.sh_addralign = 1;
    SH.sh_entsize = EntrySize;
    Out.Headers.push_back(SH);
    return static_cast<uint32_t>(Out.Headers.size() - 1);
  };
  // The table of \p Size bytes the runtime reads at \p VA; 0, the null
  // section, when it is not in the file.
  auto AddTable = [&](llvm::StringRef Name, uint32_t Type, uint64_t VA,
                      uint64_t Size, uint64_t EntrySize,
                      uint32_t Link) -> uint32_t {
    if (std::optional<uint64_t> Offset = FileOffset(Name, VA, Size)) {
      if (Type == SHT_STRTAB &&
          (Size == 0 || ELF.base()[*Offset + Size - 1] != 0)) {
        Skip(Name, "its string bytes are empty or not null terminated");
        return 0;
      }
      return Add(Name, Type, VA, *Offset, Size, EntrySize, Link);
    }
    return 0;
  };

  // glibc keeps the last PT_DYNAMIC and reads it where it is loaded.
  const Elf_Phdr *Dynamic = nullptr;
  for (const Elf_Phdr &P : *HeadersOr) {
    if (P.p_type == PT_GNU_EH_FRAME)
      AddTable(names::EhFrameHdr, SHT_PROGBITS, P.p_vaddr, P.p_filesz, 0, 0);
    if (P.p_type == PT_DYNAMIC)
      Dynamic = &P;
  }
  if (!Dynamic)
    return Out;
  const std::optional<uint64_t> DynamicOffset =
      FileOffset(names::Dynamic, Dynamic->p_vaddr, Dynamic->p_filesz);
  if (!DynamicOffset)
    return Out;

  // The dynamic linker reads entries up to DT_NULL, and a later entry for a
  // tag replaces an earlier one.
  std::map<int64_t, uint64_t> Tags;
  const size_t EntryCount = Dynamic->p_filesz / sizeof(Elf_Dyn);
  for (size_t I = 0; I < EntryCount; ++I) {
    Elf_Dyn Entry;
    std::memcpy(&Entry, ELF.base() + *DynamicOffset + I * sizeof(Elf_Dyn),
                sizeof(Entry));
    if (Entry.d_tag == DT_NULL)
      break;
    Tags[Entry.d_tag] = Entry.d_un.d_val;
  }
  auto Tag = [&](int64_t Name) -> std::optional<uint64_t> {
    if (auto It = Tags.find(Name); It != Tags.end())
      return It->second;
    return std::nullopt;
  };
  auto Word = [&](uint64_t Offset) {
    return llvm::support::endian::read32le(ELF.base() + Offset);
  };

  // How many dynamic symbols there are.  Only a hash table says, for the
  // dynamic linker as for us: SysV's chain has one entry per symbol, and the
  // GNU table's last chain ends at the last symbol.
  auto SymbolCount = [&]() -> std::optional<uint64_t> {
    constexpr uint64_t HeaderWords = 4;
    if (std::optional<uint64_t> Address = Tag(DT_GNU_HASH)) {
      const std::optional<uint64_t> Header =
          FileOffset(names::GnuHash, *Address, HeaderWords * 4);
      if (!Header)
        return std::nullopt;
      const uint64_t Buckets = Word(*Header), First = Word(*Header + 4),
                     BloomWords = Word(*Header + 8);
      const uint64_t BucketsAt =
          HeaderWords * 4 + BloomWords * sizeof(typename ELFT::Off);
      const std::optional<uint64_t> Table =
          FileOffset(names::GnuHash, *Address, BucketsAt + Buckets * 4);
      if (!Table)
        return std::nullopt;
      uint64_t Last = 0;
      for (uint64_t I = 0; I < Buckets; ++I)
        Last = std::max<uint64_t>(Last, Word(*Table + BucketsAt + I * 4));
      if (Last == 0)
        return First;
      if (Last < First) {
        Skip(names::Dynsym, "a GNU hash bucket starts before its symbols");
        return std::nullopt;
      }
      const uint64_t ChainAt = *Table + BucketsAt + Buckets * 4;
      for (uint64_t Index = Last;; ++Index) {
        const uint64_t At = ChainAt + (Index - First) * 4;
        if (At > FileSize - 4) {
          Skip(names::Dynsym, "its GNU hash chain runs past the file");
          return std::nullopt;
        }
        if (Word(At) & 1)
          return Index + 1;
      }
    }
    if (std::optional<uint64_t> Address = Tag(DT_HASH)) {
      const std::optional<uint64_t> Header =
          FileOffset(names::Hash, *Address, 8);
      if (!Header)
        return std::nullopt;
      return Word(*Header + 4);
    }
    Skip(names::Dynsym, "no hash table bounds it");
    return std::nullopt;
  };

  uint32_t DynStr = 0;
  if (std::optional<uint64_t> Address = Tag(DT_STRTAB))
    DynStr = AddTable(names::Dynstr, SHT_STRTAB, *Address,
                      Tag(DT_STRSZ).value_or(0), 0, 0);
  Add(names::Dynamic, SHT_DYNAMIC, Dynamic->p_vaddr, *DynamicOffset,
      EntryCount * sizeof(Elf_Dyn), sizeof(Elf_Dyn), DynStr);

  uint32_t DynSym = 0;
  if (std::optional<uint64_t> Address = Tag(DT_SYMTAB)) {
    using Elf_Sym = typename ELFT::Sym;
    if (Tag(DT_SYMENT).value_or(sizeof(Elf_Sym)) != sizeof(Elf_Sym))
      Skip(names::Dynsym, "DT_SYMENT is not the size of a symbol");
    else if (std::optional<uint64_t> Count = SymbolCount();
             Count && *Count > FileSize / sizeof(Elf_Sym))
      Skip(names::Dynsym, "its hash table counts more symbols than fit");
    else if (Count)
      DynSym = AddTable(names::Dynsym, SHT_DYNSYM, *Address,
                        *Count * sizeof(Elf_Sym), sizeof(Elf_Sym), DynStr);
  }

  // The PLT relocations, which DT_RELASZ or DT_RELSZ may also count when they
  // end the table; glibc then reads them once, as PLT relocations.
  const std::optional<uint64_t> PltAddress = Tag(DT_JMPREL);
  const uint64_t PltSize = Tag(DT_PLTRELSZ).value_or(0);
  auto AddRelocations = [&](llvm::StringRef Name, uint32_t Type,
                            int64_t AddressTag, int64_t SizeTag,
                            std::optional<int64_t> EntryTag,
                            uint64_t EntrySize) {
    const std::optional<uint64_t> Address = Tag(AddressTag);
    uint64_t Size = Tag(SizeTag).value_or(0);
    if (!Address || !Size)
      return;
    if (EntryTag && Tag(*EntryTag).value_or(EntrySize) != EntrySize) {
      Skip(Name, "its entry size tag is not the size of its entries");
      return;
    }
    if (PltAddress && PltSize && *PltAddress >= *Address &&
        *PltAddress - *Address < Size) {
      if (PltSize != Size - (*PltAddress - *Address)) {
        Skip(Name, "the PLT relocations overlap it without ending it");
        return;
      }
      Size = *PltAddress - *Address;
    }
    if (!Size)
      return;
    if (Size % EntrySize) {
      Skip(Name, "it is not a whole number of entries");
      return;
    }
    AddTable(Name, Type, *Address, Size, EntrySize, DynSym);
  };
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;
  using Elf_Relr = typename ELFT::Relr;
  // Android's packed tables are byte streams.
  constexpr uint64_t PackedEntrySize = 1;
  AddRelocations(names::RelaDyn, SHT_RELA, DT_RELA, DT_RELASZ, DT_RELAENT,
                 sizeof(Elf_Rela));
  AddRelocations(names::RelDyn, SHT_REL, DT_REL, DT_RELSZ, DT_RELENT,
                 sizeof(Elf_Rel));
  AddRelocations(names::RelaDyn, SHT_ANDROID_RELA, DT_ANDROID_RELA,
                 DT_ANDROID_RELASZ, std::nullopt, PackedEntrySize);
  AddRelocations(names::RelDyn, SHT_ANDROID_REL, DT_ANDROID_REL,
                 DT_ANDROID_RELSZ, std::nullopt, PackedEntrySize);
  AddRelocations(names::RelrDyn, SHT_RELR, DT_RELR, DT_RELRSZ, DT_RELRENT,
                 sizeof(Elf_Relr));
  AddRelocations(names::RelrDyn, SHT_ANDROID_RELR, DT_ANDROID_RELR,
                 DT_ANDROID_RELRSZ, DT_ANDROID_RELRENT, sizeof(Elf_Relr));
  if (PltAddress && PltSize) {
    const std::optional<uint64_t> Kind = Tag(DT_PLTREL);
    const bool IsRela = Kind == uint64_t(DT_RELA);
    const uint64_t EntrySize = IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    const llvm::StringRef Name = IsRela ? names::RelaPlt : names::RelPlt;
    if (!IsRela && Kind != uint64_t(DT_REL))
      Skip(Name, "DT_PLTREL names neither REL nor RELA");
    else if (PltSize % EntrySize)
      Skip(Name, "it is not a whole number of entries");
    else
      AddTable(Name, IsRela ? SHT_RELA : SHT_REL, *PltAddress, PltSize,
               EntrySize, DynSym);
  }
  return Out;
}

template DynamicSections<llvm::object::ELF32LE>
reconstructDynamicSections<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &, BinaryImage &);
template DynamicSections<llvm::object::ELF64LE>
reconstructDynamicSections<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &, BinaryImage &);

} // namespace detail
} // namespace elf_loader
} // namespace neverd
