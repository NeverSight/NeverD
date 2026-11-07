//===- MachOObjectExterns.cpp - Addresses of a Mach-O object's externs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collects what a Mach-O object references without defining: the undefined
/// symbols its relocations name, whether a call or branch reaches each, its
/// common symbols (an undefined external whose value is its size), and the
/// symbols its GOT references reach.
///
//===----------------------------------------------------------------------===//

#include "MachOObjectExterns.h"

#include "neverd/loader/PointerRelocation.h"
#include "neverd/object/SectionNames.h"

#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace neverd::macho_loader {

namespace {

using llvm::object::DataRefImpl;
using llvm::object::MachOObjectFile;

struct NList {
  uint8_t Type = 0;
  uint8_t Sect = 0;
  uint16_t Desc = 0;
  uint64_t Value = 0;
};

NList nlistOf(const MachOObjectFile &Obj, DataRefImpl DRI) {
  if (Obj.is64Bit()) {
    const llvm::MachO::nlist_64 Entry = Obj.getSymbol64TableEntry(DRI);
    return {Entry.n_type, Entry.n_sect, Entry.n_desc, Entry.n_value};
  }
  const llvm::MachO::nlist Entry = Obj.getSymbolTableEntry(DRI);
  return {Entry.n_type, Entry.n_sect, static_cast<uint16_t>(Entry.n_desc),
          Entry.n_value};
}

bool isUndefined(const NList &N) {
  return (N.Type & llvm::MachO::N_TYPE) == llvm::MachO::N_UNDF;
}

/// An undefined external whose value is not zero is a common symbol of that
/// many bytes.
bool isCommon(const NList &N) {
  return isUndefined(N) && N.Value != 0 && (N.Type & llvm::MachO::N_EXT) != 0;
}

uint64_t commonAlignment(const NList &N) {
  return uint64_t(1) << llvm::MachO::GET_COMM_ALIGN(N.Desc);
}

std::optional<std::string> nameOf(const MachOObjectFile &Obj, DataRefImpl DRI) {
  auto NameOr = Obj.getSymbolName(DRI);
  if (!NameOr) {
    llvm::consumeError(NameOr.takeError());
    return std::nullopt;
  }
  return NameOr->str();
}

} // namespace

bool isBranchReference(Arch A, uint32_t Type) {
  using namespace llvm::MachO;
#define NEVERD_MACHO_BRANCH_REFERENCE(TheArch, BranchType)                     \
  if (A == Arch::TheArch && Type == BranchType)                                \
    return true;
#include "MachOObjectRelocations.def"
  return false;
}

std::optional<uint32_t> directTypeOfGOTReference(Arch A, uint32_t Type) {
  using namespace llvm::MachO;
#define NEVERD_MACHO_GOT_REFERENCE(TheArch, GotType, DirectType)               \
  if (A == Arch::TheArch && Type == GotType)                                   \
    return DirectType;
#include "MachOObjectRelocations.def"
  return std::nullopt;
}

bool isGOTPointerReference(Arch A, uint32_t Type) {
  using namespace llvm::MachO;
#define NEVERD_MACHO_GOT_POINTER(TheArch, PointerType)                         \
  if (A == Arch::TheArch && Type == PointerType)                               \
    return true;
#include "MachOObjectRelocations.def"
  return false;
}

llvm::Expected<ObjectExterns> planObjectExterns(const MachOObjectFile &Obj,
                                                const BinaryImage &Img) {
  object_externs::ExternRequests Requests;
  std::vector<uint32_t> Cells;
  std::set<uint32_t> SeenCells;
  for (const llvm::object::SectionRef &Sec : Obj.sections()) {
    for (const llvm::object::RelocationRef &Reloc : Sec.relocations()) {
      const auto Raw = Obj.getRelocation(Reloc.getRawDataRefImpl());
      if (Obj.isRelocationScattered(Raw) ||
          !Obj.getPlainRelocationExternal(Raw))
        continue;
      const uint32_t Type = Obj.getAnyRelocationType(Raw);
      const uint32_t Index = Obj.getPlainRelocationSymbolNum(Raw);
      if ((directTypeOfGOTReference(Img.Arch, Type) ||
           isGOTPointerReference(Img.Arch, Type)) &&
          SeenCells.insert(Index).second)
        Cells.push_back(Index);
      const llvm::object::symbol_iterator Sym = Obj.getSymbolByIndex(Index);
      if (Sym == Obj.symbol_end())
        continue;
      const DataRefImpl DRI = Sym->getRawDataRefImpl();
      const NList N = nlistOf(Obj, DRI);
      if (!isUndefined(N))
        continue;
      const std::optional<std::string> Name = nameOf(Obj, DRI);
      if (!Name)
        continue;
      // A weak reference's address is the null test the code makes of it,
      // which any address the loader gave it would decide.
      if (isCommon(N))
        Requests.noteCommon(*Name, N.Value, commonAlignment(N));
      else if (N.Value == 0 && (N.Desc & llvm::MachO::N_WEAK_REF) == 0)
        Requests.noteUndefined(*Name, isBranchReference(Img.Arch, Type), 0);
    }
  }
  // A linker allocates every common symbol, named by a relocation or not.
  for (const llvm::object::SymbolRef &Sym : Obj.symbols()) {
    const DataRefImpl DRI = Sym.getRawDataRefImpl();
    const NList N = nlistOf(Obj, DRI);
    if (!isCommon(N))
      continue;
    if (const std::optional<std::string> Name = nameOf(Obj, DRI))
      Requests.noteCommon(*Name, N.Value, commonAlignment(N));
  }
  va_t ImageEnd = 0;
  for (const Segment &Seg : Img.Segments)
    if (Seg.Size <= InvalidVA - Seg.VA)
      ImageEnd = std::max<va_t>(ImageEnd, Seg.VA + Seg.Size);
  for (const Section &Sec : Img.Sections)
    if (Sec.Size <= InvalidVA - Sec.VA)
      ImageEnd = std::max<va_t>(ImageEnd, Sec.VA + Sec.Size);
  auto LayoutOr = object_externs::layoutExterns(
      Requests, Cells.size(), Obj.is64Bit() ? 8 : 4, ImageEnd);
  if (!LayoutOr)
    return LayoutOr.takeError();
  ObjectExterns Externs;
  static_cast<object_externs::ExternLayout &>(Externs) = std::move(*LayoutOr);
  for (size_t I = 0; I < Cells.size(); ++I)
    Externs.GOTCells[Cells[I]] = Externs.cellAddress(I);
  return Externs;
}

void addObjectExterns(const MachOObjectFile &Obj, const ObjectExterns &Externs,
                      BinaryImage &Img) {
  object_externs::addExternSegment(Externs, Img);
  // The symbol table publishes no undefined symbol, so the common ones are
  // published here, at their storage.
  for (const auto &[Name, StorageVA] : Externs.CommonSlots) {
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = StorageVA;
    if (auto It = Externs.CommonSizes.find(Name);
        It != Externs.CommonSizes.end())
      Sym.Size = It->second;
    Img.Symbols.push_back(std::move(Sym));
  }
  if (Externs.GOTCells.empty())
    return;
  // A GOT entry holds its symbol's address: the extern address of an
  // undefined or common symbol, where the object places a defined one.
  std::vector<uint8_t> Contents;
  std::vector<std::pair<va_t, std::string>> ImportEntries;
  std::vector<std::pair<va_t, std::pair<va_t, va_t>>> PointerEntries;
  for (const auto &[Index, CellVA] : Externs.GOTCells) {
    const llvm::object::symbol_iterator Sym = Obj.getSymbolByIndex(Index);
    if (Sym == Obj.symbol_end())
      continue;
    const DataRefImpl DRI = Sym->getRawDataRefImpl();
    const NList N = nlistOf(Obj, DRI);
    const std::optional<std::string> Name = nameOf(Obj, DRI);
    if (!Name)
      continue;
    std::optional<va_t> Address;
    va_t OwnerVA = InvalidVA;
    if (isCommon(N)) {
      if (auto It = Externs.CommonSlots.find(*Name);
          It != Externs.CommonSlots.end()) {
        Address = It->second;
        OwnerVA = Externs.ExternBase;
      }
    } else if (isUndefined(N)) {
      // The entry of a called extern holds the import, so a call through it
      // reaches the import; data's holds the address of its storage here.
      if (auto It = Externs.SymbolSlots.find(*Name);
          It != Externs.SymbolSlots.end()) {
        Address = It->second;
        if (Externs.CalledSymbols.count(*Name)) {
          ImportEntries.push_back({CellVA, *Name});
        } else {
          OwnerVA = Externs.ExternBase;
          Img.ObjectExternDataCells[CellVA] = *Name;
        }
      }
    } else if ((N.Type & llvm::MachO::N_TYPE) == llvm::MachO::N_SECT &&
               N.Sect > 0 && N.Sect <= Img.Sections.size()) {
      // An object's sections keep their addresses; a Thumb function's
      // address carries its mode in bit 0.
      Address = N.Value | ((N.Desc & llvm::MachO::N_ARM_THUMB_DEF) ? 1 : 0);
      OwnerVA = Img.Sections[N.Sect - 1].VA;
    } else if ((N.Type & llvm::MachO::N_TYPE) == llvm::MachO::N_ABS) {
      Address = N.Value;
    }
    if (!Address)
      continue;
    object_externs::writeCell(
        Externs, Contents,
        static_cast<size_t>((CellVA - Externs.CellBase) / Externs.PointerBytes),
        *Address);
    // An undefined data symbol's cell binds that symbol, as a linked GOT
    // slot does (collectDataSymbolBindings): it points into no storage of
    // the object's.
    if (OwnerVA != InvalidVA && !Img.ObjectExternDataCells.count(CellVA))
      PointerEntries.push_back({CellVA, {*Address, OwnerVA}});
  }
  object_externs::addCellSegment(Externs, section_names::macho::Got,
                                 std::move(Contents), Img);
  for (const auto &[CellVA, Name] : ImportEntries)
    Img.recordImportStorageSlot(CellVA, Name, 0,
                                ImportStorageEvidence::LoaderBind);
  for (const auto &[CellVA, Target] : PointerEntries)
    recordAbsolutePointerRelocation(Img, CellVA, Target.first, Target.second);
}

std::optional<std::pair<va_t, va_t>>
resolveObjectExtern(const MachOObjectFile &Obj, const ObjectExterns &Externs,
                    llvm::object::symbol_iterator Sym, bool Branch) {
  if (Sym == Obj.symbol_end())
    return std::nullopt;
  const DataRefImpl DRI = Sym->getRawDataRefImpl();
  const NList N = nlistOf(Obj, DRI);
  if (!isUndefined(N))
    return std::nullopt;
  const std::optional<std::string> Name = nameOf(Obj, DRI);
  if (!Name)
    return std::nullopt;
  if (isCommon(N)) {
    if (auto It = Externs.CommonSlots.find(*Name);
        It != Externs.CommonSlots.end())
      return std::make_pair(It->second, Externs.ExternBase);
    return std::nullopt;
  }
  if (auto It = Externs.SymbolSlots.find(*Name);
      It != Externs.SymbolSlots.end())
    return std::make_pair(It->second, Branch ? InvalidVA : Externs.ExternBase);
  return std::nullopt;
}

} // namespace neverd::macho_loader
