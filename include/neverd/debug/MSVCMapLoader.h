//===- MSVCMapLoader.h - MSVC linker MAP file loader ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses MSVC linker MAP files (link.exe /MAP) to extract function
/// symbol addresses for stripped PE binaries.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_MSVCMAPLOADER_H
#define NEVERD_DEBUG_MSVCMAPLOADER_H

#include "neverd/debug/MapDebugContextBase.h"

#include <filesystem>
#include <memory>

namespace neverd {

class MSVCMapDebugContext : public MapDebugContextBase {
public:
  static std::unique_ptr<MSVCMapDebugContext>
  load(const std::filesystem::path &MapPath, uint64_t ImageBase);
};

} // namespace neverd

#endif // NEVERD_DEBUG_MSVCMAPLOADER_H
