//===- ELFIntegerProfile.h - RISC-V and PowerPC ELF profile admission -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate the native integer profile for both ELF identification and loading.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_ELF_ELFINTEGERPROFILE_H
#define NEVERD_LIB_LOADER_ELF_ELFINTEGERPROFILE_H

#include "neverd/Common.h"
#include "neverd/lift/RISCV/RISCVProfile.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <optional>

namespace llvm::object {
class ObjectFile;
}
namespace neverd {
struct BinaryImage;
namespace elf_loader {
struct PPC64LinkageTables {
  va_t Address = 0;
  uint64_t Size = 0;
  unsigned EntrySize = 0;
  bool HasDynamicBounds = false;
  va_t JumpRelocations = 0;
  uint64_t JumpRelocationSize = 0;
  std::optional<va_t> Glink = std::nullopt;
  bool PLTLocalEntry = false;
};
llvm::Error readIntegerProfile(const llvm::object::ObjectFile &Object, Arch &A,
                               RISCVProfile &Profile,
                               PPC64LinkageTables *Linkage = nullptr);
llvm::Error readRISCVLinkage(BinaryImage &Image,
                             llvm::ArrayRef<va_t> RelrSlots = {});
llvm::Error readPPC64Descriptors(BinaryImage &Image, PPC64LinkageTables Table);
llvm::Error recordPPC64ELFv2Entry(BinaryImage &Image, va_t Entry, uint64_t Size,
                                  uint8_t Other);
llvm::Error readPPC64Linkage(BinaryImage &Image,
                             const PPC64LinkageTables &Table);
struct PPC32PLTTable {
  va_t Address = 0;
  va_t Relocations = 0;
  uint64_t Count = 0;
  bool Secure = false;
};
llvm::Expected<PPC32PLTTable> readPPC32PLTTable(const BinaryImage &Image);
llvm::Error readPPC32Linkage(BinaryImage &Image, const PPC32PLTTable &Table);
} // namespace elf_loader
} // namespace neverd
#endif // NEVERD_LIB_LOADER_ELF_ELFINTEGERPROFILE_H
