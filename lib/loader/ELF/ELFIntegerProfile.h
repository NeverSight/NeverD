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

#include "llvm/Support/Error.h"

#include <optional>

namespace llvm::object {
class ObjectFile;
}
namespace neverd {
struct BinaryImage;
namespace elf_loader {
struct PPC64DescriptorTable {
  va_t Address = 0;
  uint64_t Size = 0;
  unsigned EntrySize = 0;
  bool HasDynamicBounds = false;
  va_t JumpRelocations = 0;
  uint64_t JumpRelocationSize = 0;
  std::optional<va_t> Glink = std::nullopt;
};
llvm::Error readIntegerProfile(const llvm::object::ObjectFile &Object, Arch &A,
                               RISCVProfile &Profile,
                               PPC64DescriptorTable *Descriptors = nullptr);
llvm::Error readPPC64Descriptors(BinaryImage &Image,
                                 PPC64DescriptorTable Table);
llvm::Error readPPC64Linkage(BinaryImage &Image,
                             const PPC64DescriptorTable &Table);
} // namespace elf_loader
} // namespace neverd
#endif // NEVERD_LIB_LOADER_ELF_ELFINTEGERPROFILE_H
