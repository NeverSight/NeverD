//===- COFFLoader.h - COFF/PE binary format loader ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the COFFLoader class which parses PE/COFF binaries (both
/// PE32 and PE32+) into a BinaryImage using LLVM's Object/COFF API.
/// Includes import/export table resolution, exception directory
/// processing, and heuristic function discovery.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFLOADER_H
#define NEVERD_LOADER_COFF_COFFLOADER_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <cstddef>

namespace neverd {

class COFFLoader : public Loader {
public:
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;
  llvm::Expected<BinaryImage> loadBuffer(llvm::MemoryBufferRef Buffer) override;
  /// Add the load dialog's rows for \p Buffer, whose file magic this
  /// loader reads (LoadCandidate.h), judged as load() judges the file.
  static void identify(llvm::MemoryBufferRef Buffer,
                       std::vector<LoadCandidate> &Rows);
  /// Check the mapping inventory of a full PE image against its independently
  /// validated section count. An optional appended header mapping must retain
  /// its exact original bytes and read-only identity. This does not validate
  /// the execution semantics of the real sections.
  static bool hasExpectedPEMappingInventory(const BinaryImage &Image,
                                            std::size_t SectionCount);
};

} // namespace neverd

#endif // NEVERD_LOADER_COFF_COFFLOADER_H
