#ifndef NEVERD_LOADER_MACHO_DARWINRUNTIMEIMPORT_H
#define NEVERD_LOADER_MACHO_DARWINRUNTIMEIMPORT_H

#include "neverd/loader/BinaryImage.h"

namespace neverd {
/// Match an exact install name or an alias explicitly present in an SDK row.
inline bool darwinExportModuleMatches(llvm::StringRef Modules,
                                      llvm::StringRef Module) {
  if (Module.empty())
    return false;
  while (!Modules.empty()) {
    const auto [Current, Remaining] = Modules.split('|');
    if (Current == Module)
      return true;
    Modules = Remaining;
  }
  return false;
}

/// A source call must name one exact, non-weak runtime import. Preserve the
/// original linker spelling for the runtime-specific ABI catalog to match.
inline std::optional<llvm::StringRef>
darwinRuntimeImport(const BinaryImage &Image, va_t Slot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(Slot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(Slot);
  if (Import == Image.ImportPtrSlots.end())
    return std::nullopt;
  if (auto I = Image.ImportStorageSlots.find(Slot);
      I != Image.ImportStorageSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend))
    return std::nullopt;
  if (auto I = Image.DyldBindSlots.find(Slot);
      I != Image.DyldBindSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend ||
       I->second.WeakImport))
    return std::nullopt;
  return Import->second;
}

/// Match one exact weak runtime import. Weak data and function declarations
/// must preserve this linkage so a missing optional symbol still evaluates to
/// null at runtime.
inline std::optional<llvm::StringRef>
darwinWeakRuntimeImport(const BinaryImage &Image, va_t Slot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(Slot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(Slot);
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Import == Image.ImportPtrSlots.end() ||
      Bind == Image.DyldBindSlots.end() ||
      Bind->second.Name != Import->second || Bind->second.Addend ||
      !Bind->second.WeakImport)
    return std::nullopt;
  if (auto I = Image.ImportStorageSlots.find(Slot);
      I != Image.ImportStorageSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend))
    return std::nullopt;
  return Import->second;
}
} // namespace neverd
#endif
