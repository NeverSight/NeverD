//===- ELFLoaderExterns.cpp - Addresses of an object's externs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What an ELF relocatable object references without defining: the undefined
/// symbols its relocations name, whether a call or branch reaches each, and
/// its common symbols, which the shared object-extern layer places in an
/// `extern` segment past every section (neverd/loader/ObjectExterns.h).  A GOT
/// reference reaches its symbol through an entry the linker would create; the
/// loader makes those entries as the layer's cells, a read-only `.got`, each
/// holding its symbol's address.
///
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"

#include "neverd/loader/PointerRelocation.h"
#include "neverd/object/SectionNames.h"

#include "llvm/BinaryFormat/ELF.h"

#include <cstring>
#include <set>
#include <string>

namespace neverd {
namespace elf_loader {
namespace detail {

std::optional<uint32_t> directTypeOfGOTReference(Arch A, uint32_t Type) {
  using namespace llvm::ELF;
#define NEVERD_ELF_GOT_REFERENCE(TheArch, GotType, DirectType)                 \
  if (A == Arch::TheArch && Type == GotType)                                   \
    return DirectType;
#include "ELFObjectRelocations.def"
  return std::nullopt;
}

bool isBranchReference(Arch A, uint32_t Type) {
  using namespace llvm::ELF;
#define NEVERD_ELF_BRANCH_REFERENCE(TheArch, BranchType)                       \
  if (A == Arch::TheArch && Type == BranchType)                                \
    return true;
#include "ELFObjectRelocations.def"
  return false;
}

namespace {

/// The `_GLOBAL_OFFSET_TABLE_` an i386 or ARM object addresses its GOT from
/// is the linker's, not a function or datum of another module.
constexpr llvm::StringLiteral kGlobalOffsetTable("_GLOBAL_OFFSET_TABLE_");

/// One relocation of an allocated section, as the extern plan reads it.
struct ObjectRelocation {
  uint32_t SymbolTable = 0;
  uint32_t SymbolIndex = 0;
  uint32_t Type = 0;
  /// A REL relocation's addend is in the relocated field; it reads as zero.
  int64_t Addend = 0;
  bool InCode = false;
};

/// Calls \p Visit(Relocation, Symbol, Name) for every relocation of
/// \p Sections that applies to an allocated section and names a symbol.
template <typename ELFT, typename Fn>
void forEachObjectRelocation(const llvm::object::ELFFile<ELFT> &ELF,
                             llvm::ArrayRef<typename ELFT::Shdr> Sections,
                             const uint8_t *Data, size_t Size, Fn Visit) {
  using namespace llvm::ELF;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;
  for (const auto &SH : Sections) {
    const bool IsRela = SH.sh_type == SHT_RELA;
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    const size_t EntrySize = IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    if (SH.sh_entsize < EntrySize ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;
    const auto *ApplySH = getShdr<ELFT>(Sections, SH.sh_info);
    const auto *SymSH = getShdr<ELFT>(Sections, SH.sh_link);
    if (!ApplySH || !(ApplySH->sh_flags & SHF_ALLOC) || !SymSH)
      continue;
    auto SymsOr = ELF.symbols(SymSH);
    auto StrTabOr = ELF.getStringTableForSymtab(*SymSH);
    if (!SymsOr || !StrTabOr) {
      if (!SymsOr)
        llvm::consumeError(SymsOr.takeError());
      if (!StrTabOr)
        llvm::consumeError(StrTabOr.takeError());
      continue;
    }
    ObjectRelocation Rel;
    Rel.SymbolTable = SH.sh_link;
    Rel.InCode = (ApplySH->sh_flags & SHF_EXECINSTR) != 0;
    const size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      const uint64_t Off =
          SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
      if (!rangeInBounds(Off, EntrySize, Size))
        break;
      if (IsRela) {
        Elf_Rela R;
        std::memcpy(&R, Data + static_cast<size_t>(Off), sizeof(R));
        Rel.Type = R.getType(false);
        Rel.SymbolIndex = R.getSymbol(false);
        Rel.Addend = R.r_addend;
      } else {
        Elf_Rel R;
        std::memcpy(&R, Data + static_cast<size_t>(Off), sizeof(R));
        Rel.Type = R.getType(false);
        Rel.SymbolIndex = R.getSymbol(false);
        Rel.Addend = 0;
      }
      if (Rel.SymbolIndex == 0 || Rel.SymbolIndex >= SymsOr->size())
        continue;
      const auto &Sym = (*SymsOr)[Rel.SymbolIndex];
      llvm::StringRef Name;
      if (auto NameOr = Sym.getName(*StrTabOr))
        Name = *NameOr;
      else
        llvm::consumeError(NameOr.takeError());
      Visit(Rel, Sym, Name);
    }
  }
}

} // namespace

template <typename ELFT>
llvm::Expected<ObjectExterns>
planObjectExterns(const llvm::object::ELFFile<ELFT> &ELF,
                  llvm::ArrayRef<typename ELFT::Shdr> Sections,
                  const uint8_t *Data, size_t Size, Arch A, va_t ImageEnd) {
  using namespace llvm::ELF;
  object_externs::ExternRequests Requests;
  std::vector<std::pair<uint32_t, uint32_t>> Referenced;
  std::set<std::pair<uint32_t, uint32_t>> SeenReferenced;
  // A common symbol's value is its alignment.
  auto NoteCommon = [&](const typename ELFT::Sym &Sym, llvm::StringRef Name) {
    Requests.noteCommon(Name, Sym.st_size, Sym.st_value);
  };
  forEachObjectRelocation<ELFT>(
      ELF, Sections, Data, Size,
      [&](const ObjectRelocation &Rel, const typename ELFT::Sym &Sym,
          llvm::StringRef Name) {
        if (directTypeOfGOTReference(A, Rel.Type) &&
            SeenReferenced.insert({Rel.SymbolTable, Rel.SymbolIndex}).second)
          Referenced.push_back({Rel.SymbolTable, Rel.SymbolIndex});
        // A weak undefined symbol's address is the null test the code makes
        // of it, which any address the loader gave it would decide.
        if (Sym.st_shndx == SHN_COMMON)
          NoteCommon(Sym, Name);
        else if (Sym.st_shndx == SHN_UNDEF && Name != kGlobalOffsetTable &&
                 Sym.getBinding() != STB_WEAK)
          Requests.noteUndefined(
              Name, Rel.InCode && isBranchReference(A, Rel.Type), Rel.Addend);
      });
  // A linker allocates every common symbol, named by a relocation or not.
  for (const auto &SH : Sections) {
    if (SH.sh_type != SHT_SYMTAB)
      continue;
    auto SymsOr = ELF.symbols(&SH);
    auto StrTabOr = ELF.getStringTableForSymtab(SH);
    if (!SymsOr || !StrTabOr) {
      if (!SymsOr)
        llvm::consumeError(SymsOr.takeError());
      if (!StrTabOr)
        llvm::consumeError(StrTabOr.takeError());
      continue;
    }
    for (const auto &Sym : *SymsOr) {
      if (Sym.st_shndx != SHN_COMMON)
        continue;
      auto NameOr = Sym.getName(*StrTabOr);
      if (!NameOr) {
        llvm::consumeError(NameOr.takeError());
        continue;
      }
      NoteCommon(Sym, *NameOr);
    }
  }
  auto LayoutOr = object_externs::layoutExterns(
      Requests, Referenced.size(), sizeof(typename ELFT::Addr), ImageEnd);
  if (!LayoutOr)
    return LayoutOr.takeError();
  ObjectExterns Externs;
  static_cast<object_externs::ExternLayout &>(Externs) = std::move(*LayoutOr);
  for (size_t I = 0; I < Referenced.size(); ++I)
    Externs.GOTEntries[Referenced[I]] = Externs.cellAddress(I);
  return Externs;
}

template <typename ELFT>
void addObjectExterns(const llvm::object::ELFFile<ELFT> &ELF,
                      llvm::ArrayRef<typename ELFT::Shdr> Sections,
                      const std::vector<va_t> &SecBase,
                      const ObjectExterns &Externs, BinaryImage &Img) {
  using namespace llvm::ELF;
  object_externs::addExternSegment(Externs, Img);
  if (Externs.GOTEntries.empty())
    return;
  // A GOT entry holds its symbol's address: the extern address of an
  // undefined or common symbol, where the object places a defined one.
  std::vector<uint8_t> GOT;
  std::vector<std::pair<va_t, std::string>> ImportEntries;
  std::vector<std::pair<va_t, std::pair<va_t, va_t>>> PointerEntries;
  size_t Index = 0;
  for (const auto &[Key, EntryVA] : Externs.GOTEntries) {
    Index = static_cast<size_t>((EntryVA - Externs.CellBase) /
                                Externs.PointerBytes);
    const auto *SymSH = getShdr<ELFT>(Sections, Key.first);
    if (!SymSH)
      continue;
    auto SymsOr = ELF.symbols(SymSH);
    auto StrTabOr = ELF.getStringTableForSymtab(*SymSH);
    if (!SymsOr || !StrTabOr || Key.second >= SymsOr->size()) {
      if (!SymsOr)
        llvm::consumeError(SymsOr.takeError());
      if (!StrTabOr)
        llvm::consumeError(StrTabOr.takeError());
      continue;
    }
    const auto &Sym = (*SymsOr)[Key.second];
    std::string Name;
    if (auto NameOr = Sym.getName(*StrTabOr))
      Name = NameOr->str();
    else
      llvm::consumeError(NameOr.takeError());
    std::optional<va_t> Address;
    va_t OwnerVA = InvalidVA;
    if (Sym.st_shndx == SHN_UNDEF) {
      // The entry of a called extern holds the import, so a call through it
      // reaches the import; data's holds the address of its storage here.
      if (auto It = Externs.SymbolSlots.find(Name);
          It != Externs.SymbolSlots.end()) {
        Address = It->second;
        if (Externs.CalledSymbols.count(Name)) {
          ImportEntries.push_back({EntryVA, Name});
        } else {
          OwnerVA = Externs.ExternBase;
          Img.ObjectExternDataCells[EntryVA] = Name;
        }
      }
    } else if (Sym.st_shndx == SHN_COMMON) {
      if (auto It = Externs.CommonSlots.find(Name);
          It != Externs.CommonSlots.end()) {
        Address = It->second;
        OwnerVA = Externs.ExternBase;
      }
    } else if (Sym.st_shndx == SHN_ABS) {
      Address = static_cast<va_t>(Sym.st_value);
    } else if (Sym.st_shndx < SHN_LORESERVE && Sym.st_shndx < SecBase.size() &&
               static_cast<va_t>(Sym.st_value) <=
                   InvalidVA - SecBase[Sym.st_shndx]) {
      Address = SecBase[Sym.st_shndx] + static_cast<va_t>(Sym.st_value);
      OwnerVA = SecBase[Sym.st_shndx];
    }
    if (!Address)
      continue;
    object_externs::writeCell(Externs, GOT, Index, *Address);
    // An undefined data symbol's entry binds that symbol, as a linked GOT
    // slot does (collectDataSymbolBindings): it points into no storage of
    // the object's.
    if (OwnerVA != InvalidVA && !Img.ObjectExternDataCells.count(EntryVA))
      PointerEntries.push_back({EntryVA, {*Address, OwnerVA}});
  }
  object_externs::addCellSegment(Externs, section_names::elf::Got,
                                 std::move(GOT), Img);
  for (const auto &[EntryVA, Name] : ImportEntries)
    Img.recordImportStorageSlot(EntryVA, Name, 0,
                                ImportStorageEvidence::LoaderBind);
  for (const auto &[EntryVA, Target] : PointerEntries)
    recordAbsolutePointerRelocation(Img, EntryVA, Target.first, Target.second);
}

template llvm::Expected<ObjectExterns> planObjectExterns<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t, Arch,
    va_t);
template llvm::Expected<ObjectExterns> planObjectExterns<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t, Arch,
    va_t);
template void addObjectExterns<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const std::vector<va_t> &,
    const ObjectExterns &, BinaryImage &);
template void addObjectExterns<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const std::vector<va_t> &,
    const ObjectExterns &, BinaryImage &);

} // namespace detail
} // namespace elf_loader
} // namespace neverd
