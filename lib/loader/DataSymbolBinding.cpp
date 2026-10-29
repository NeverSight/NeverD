//===- DataSymbolBinding.cpp - Symbol-bound data pointer storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/loader/DataSymbolBinding.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace neverd {
namespace {
bool isDataSlot(Arch Target, uint32_t Type) {
#define NEVERD_ELF_DATA_SLOT(ISA, Reloc)                                       \
  if (Target == Arch::ISA && Type == llvm::ELF::Reloc)                         \
    return true;
#include "ELF/ELFDynamicRelocations.def"
  return false;
}

bool isAbsoluteSymbol(Arch Target, uint32_t Type) {
#define NEVERD_ELF_ABSOLUTE_SYMBOL(ISA, Reloc)                                 \
  if (Target == Arch::ISA && Type == llvm::ELF::Reloc)                         \
    return true;
#include "ELF/ELFDynamicRelocations.def"
  return false;
}

bool immutableSlot(const BinaryImage &Image, va_t Slot) {
  const unsigned Width = Image.getPointerSize();
  return Image.readVA(Slot, Width) &&
         Image.readOnlyAfterRelocations(Slot, Width);
}
} // namespace

std::map<va_t, DataSymbolBinding>
collectDataSymbolBindings(const BinaryImage &Image) {
  std::map<va_t, DataSymbolBinding> Bindings;
  // A relocatable object's GOT cell holds the address ObjectExterns gives an
  // undefined data symbol: the cell binds that symbol, which another module
  // defines.
  if (Image.IsRelocatable) {
    for (const auto &[Cell, Name] : Image.ObjectExternDataCells) {
      DataSymbolBinding Binding;
      Binding.Name = Name;
      Binding.Immutable = immutableSlot(Image, Cell);
      Bindings.emplace(Cell, std::move(Binding));
    }
    return Bindings;
  }
  if (!Image.isELF())
    return Bindings;
  for (const auto &Relocation : Image.Relocations) {
    if (!isDataSlot(Image.Arch, Relocation.Type) || !Relocation.ELF ||
        !Relocation.ELF->Symbol)
      continue;
    const auto &Symbol = *Relocation.ELF->Symbol;
    if (!Symbol.Name || Symbol.Name->empty() ||
        (Symbol.Type != llvm::ELF::STT_OBJECT &&
         Symbol.Type != llvm::ELF::STT_NOTYPE) ||
        !Image.readVA(Relocation.Address, Image.getPointerSize()))
      continue;
    DataSymbolBinding Binding;
    Binding.Name = *Symbol.Name;
    Binding.Weak = Symbol.Binding == llvm::ELF::STB_WEAK;
    Binding.Immutable = immutableSlot(Image, Relocation.Address);
    if (Symbol.isDefined()) {
      if (Symbol.SectionIndex >= llvm::ELF::SHN_LORESERVE ||
          !Image.readVA(Symbol.Value, 1) || Image.isCodeAddress(Symbol.Value))
        continue;
      Binding.Definition = Symbol.Value;
    }
    // GLOB_DAT ignores the encoded slot for REL on x86 and ARM. AArch64
    // uses an explicit RELA addend; never borrow already relocated bytes.
    const auto UsesAddend =
        elf_loader::symbolRelocationValue(Image.Arch, Relocation.Type, 0, 1);
    if (!UsesAddend)
      throw std::invalid_argument("data relocation has no symbol-value policy");
    if (*UsesAddend != 0) {
      if (!Relocation.HasExplicitAddend)
        continue;
      Binding.Addend = Relocation.Addend;
    }
    auto [It, Inserted] = Bindings.emplace(Relocation.Address, Binding);
    if (!Inserted && It->second != Binding)
      throw std::invalid_argument(
          "conflicting data symbol relocation bindings");
  }
  return Bindings;
}

std::map<va_t, std::string>
collectExternalFunctionPointers(const BinaryImage &Image) {
  std::map<va_t, std::string> Pointers;
  const unsigned Width = Image.getPointerSize();
  if (!Image.isELF() || Image.IsRelocatable || Width == 0)
    return Pointers;
  std::set<va_t> Conflicts;
  for (const auto &Relocation : Image.Relocations) {
    if (!isAbsoluteSymbol(Image.Arch, Relocation.Type) || !Relocation.ELF ||
        !Relocation.ELF->Symbol)
      continue;
    // The loader may leave a weak symbol's word null.
    const auto &Symbol = *Relocation.ELF->Symbol;
    if (!Symbol.Name || Symbol.Name->empty() || Symbol.isDefined() ||
        !Symbol.isFunction() || Symbol.Binding == llvm::ELF::STB_WEAK)
      continue;
    const uint8_t *Bytes = Image.readVA(Relocation.Address, Width);
    if (!Bytes)
      continue;
    // RELA states the addend; REL keeps it in the word.
    const bool Exact =
        Relocation.HasExplicitAddend
            ? Relocation.Addend == 0
            : std::all_of(Bytes, Bytes + Width,
                          [](uint8_t Byte) { return Byte == 0; });
    if (!Exact)
      continue;
    auto [It, Inserted] = Pointers.emplace(Relocation.Address, *Symbol.Name);
    if (!Inserted && It->second != *Symbol.Name)
      Conflicts.insert(Relocation.Address);
  }
  for (va_t Slot : Conflicts)
    Pointers.erase(Slot);
  return Pointers;
}

namespace {
/// The code address the relocated pointer-width word at \p Slot holds, where
/// a relocation starts it at code and no data relocation also names it.
std::optional<va_t> relocatedCodeTarget(const BinaryImage &Image, va_t Slot) {
  const unsigned Width = Image.getPointerSize();
  if (Width == 0 || Width != getPointerSize(Image.is64Bit()) ||
      !Image.CodePtrRelocSlots.count(Slot) ||
      Image.DataPtrRelocSlots.count(Slot))
    return std::nullopt;
  const uint8_t *Bytes = Image.readVA(Slot, Width);
  if (!Bytes)
    return std::nullopt;
  const va_t Target = normalizeCodeAddress(readPtr(Bytes, Image.is64Bit()),
                                           Image.Arch, Image.Mode);
  if (!Image.isCodeAddress(Target))
    return std::nullopt;
  return Target;
}
} // namespace

std::map<va_t, PointerVariableCallee>
collectPointerVariableCallees(const BinaryImage &Image) {
  std::map<va_t, PointerVariableCallee> Callees;
  // readPtr reads the image's pointer width.
  const unsigned Width = Image.getPointerSize();
  if (Width == 0 || Width != getPointerSize(Image.is64Bit()))
    return Callees;
  // Import storage names its import already.
  const ImportStorageSlotCollection Imports = Image.collectImportStorageSlots();
  auto IsImportStorage = [&](va_t Slot) {
    return Imports.Slots.count(Slot) || Imports.Conflicts.count(Slot);
  };
  for (auto &[Slot, Name] : collectExternalFunctionPointers(Image))
    if (!IsImportStorage(Slot))
      Callees[Slot].ImportName = std::move(Name);
  for (const uint64_t Slot : Image.CodePtrRelocSlots) {
    if (Callees.count(Slot) || IsImportStorage(Slot))
      continue;
    if (const std::optional<va_t> Target = relocatedCodeTarget(Image, Slot))
      Callees[Slot].LocalEntry = *Target;
  }
  return Callees;
}

std::optional<va_t> ownRoutineOfImport(const BinaryImage &Image,
                                       const Import &Imp) {
  const va_t Slot = Imp.IATAddr;
  if (!Slot || !Image.CodePtrRelocSlots.count(Slot))
    return std::nullopt;
  // A slot the loader binds to another module's routine is import storage.
  const ImportStorageSlotCollection Storage =
      Image.collectImportStorageSlot(Slot);
  if (Storage.Slots.count(Slot) || Storage.Conflicts.count(Slot))
    return std::nullopt;
  return relocatedCodeTarget(Image, Slot);
}
} // namespace neverd
