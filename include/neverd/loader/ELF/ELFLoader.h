//===- ELFLoader.h - ELF binary format loader ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the ELFLoader class which parses ELF binaries (both 32-bit
/// and 64-bit, including relocatable objects) into a BinaryImage using
/// LLVM's ELF type definitions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_ELFLOADER_H
#define NEVERD_LOADER_ELF_ELFLOADER_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

namespace neverd {

class ELFLoader : public Loader {
public:
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;
  llvm::Expected<BinaryImage> loadBuffer(llvm::MemoryBufferRef Buffer) override;
  /// Add the load dialog's rows for \p Buffer, whose file magic this
  /// loader reads (LoadCandidate.h), judged as load() judges the file.
  static void identify(llvm::MemoryBufferRef Buffer,
                       std::vector<LoadCandidate> &Rows);
};

} // namespace neverd

#endif // NEVERD_LOADER_ELF_ELFLOADER_H
