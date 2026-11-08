//===- ELFMIPSProfile.h - MIPS ELF profile admission ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared profile validation for ELF identification and loading.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_ELF_ELFMIPSPROFILE_H
#define NEVERD_LIB_LOADER_ELF_ELFMIPSPROFILE_H

#include "neverd/loader/ELF/MIPSImageMetadata.h"

#include "llvm/Support/Error.h"

namespace llvm::object {
class ObjectFile;
}

namespace neverd::elf_loader {

llvm::Expected<MIPSImageMetadata>
readMIPSProfile(const llvm::object::ObjectFile &Object);

} // namespace neverd::elf_loader

#endif // NEVERD_LIB_LOADER_ELF_ELFMIPSPROFILE_H
