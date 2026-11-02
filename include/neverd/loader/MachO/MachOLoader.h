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

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace neverd {

class MachOLoader : public Loader {
public:
  MachOLoader() = default;
  explicit MachOLoader(std::string CacheImage)
      : CacheImage(std::move(CacheImage)) {}
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;
  llvm::Expected<BinaryImage> loadBuffer(llvm::MemoryBufferRef Buffer) override;
  /// Add the load dialog's rows for \p Buffer, whose file magic this
  /// loader reads (LoadCandidate.h), judged as load() judges the file.
  static void identify(llvm::MemoryBufferRef Buffer,
                       std::vector<LoadCandidate> &Rows);

private:
  std::string CacheImage;
  llvm::Expected<BinaryImage>
  loadBufferImpl(llvm::MemoryBufferRef Buffer,
                 std::shared_ptr<const macho_loader::DyldSharedCache> Cache,
                 size_t ImageIndex = 0);
};

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_MACHOLOADER_H
