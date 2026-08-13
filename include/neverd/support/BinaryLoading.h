//===- BinaryLoading.h - Binary format auto-detection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Auto-detects binary format (ELF, PE, Mach-O) by magic bytes and loads
/// via the appropriate Loader.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BINARYLOADING_H
#define NEVERD_SUPPORT_BINARYLOADING_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <filesystem>

namespace neverd {

/// Normalize every externally sourced text field in an already loaded image so
/// downstream diagnostics and JSON writers receive valid UTF-8.
void normalizeBinaryMetadata(BinaryImage &Img);

/// Auto-detect binary format from magic bytes and load via the appropriate
/// loader. Returns an error if the format is unrecognized.
llvm::Expected<BinaryImage> loadBinary(const std::filesystem::path &Path);

} // namespace neverd

#endif // NEVERD_SUPPORT_BINARYLOADING_H
