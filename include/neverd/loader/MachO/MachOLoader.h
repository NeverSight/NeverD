//===- MachOLoader.h - Mach-O binary format loader ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the MachOLoader class which parses Mach-O binaries (both
/// 32-bit and 64-bit) into a BinaryImage using LLVM's Object/MachO API.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOLOADER_H
#define NEVERD_LOADER_MACHO_MACHOLOADER_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

namespace neverd {

class MachOLoader : public Loader {
public:
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;
};

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_MACHOLOADER_H
