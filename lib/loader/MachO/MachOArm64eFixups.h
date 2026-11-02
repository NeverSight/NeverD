//===- MachOArm64eFixups.h - Shared authenticated fixups --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decodes signing recipes independently of their modern or threaded chain
/// container. Walkers validate all storage before publishing a complete plan.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_MACHOARM64EFIXUPS_H
#define NEVERD_LIB_LOADER_MACHO_MACHOARM64EFIXUPS_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/MachOArm64e.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::macho_loader::detail {

struct Arm64eImportRecord {
  std::string Name;
  std::string Module;
  std::string DisplayModule;
  int64_t Addend = 0;
  bool WeakImport = false;
  bool Valid = false;
  std::optional<int64_t> LibraryOrdinal;
};

/// Import pointers borrow a completed, immutable ordinal table until publish.
struct Arm64eFixup {
  va_t Slot;
  va_t Target = InvalidVA;
  const Arm64eImportRecord *Import = nullptr;
  int64_t Addend = 0;
  uint32_t Next = 0;
  std::optional<MachOAuthenticatedPointer> Authentication;
};

std::optional<Arm64eFixup>
decodeArm64eFixup(uint64_t Raw, const arm64e::ChainFormat &Format, va_t Slot,
                  va_t Base, llvm::ArrayRef<Arm64eImportRecord> Imports,
                  const BinaryImage &Img);

void publishArm64eFixups(llvm::ArrayRef<Arm64eFixup> Fixups, BinaryImage &Img);

void joinImportSlot(BinaryImage &Img, llvm::StringRef Name,
                    llvm::StringRef Module, va_t SlotVA);

} // namespace neverd::macho_loader::detail

#endif // NEVERD_LIB_LOADER_MACHO_MACHOARM64EFIXUPS_H
