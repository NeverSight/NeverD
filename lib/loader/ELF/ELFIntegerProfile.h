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

namespace llvm::object {
class ObjectFile;
}
namespace neverd::elf_loader {
llvm::Error readIntegerProfile(const llvm::object::ObjectFile &Object, Arch A,
                               RISCVProfile &Profile);
}
#endif // NEVERD_LIB_LOADER_ELF_ELFINTEGERPROFILE_H
