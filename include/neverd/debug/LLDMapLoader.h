//===- LLDMapLoader.h - LLD linker MAP file loader --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses linker MAP files to extract function symbol addresses.
/// Supports five formats:
///   - ELF-style  (ld.lld --Map=)
///   - COFF /MAP  (lld-link /MAP: — same as MSVC link.exe)
///   - COFF /lldmap (lld-link /lldmap:)
///   - MachO-style (ld64.lld -map)
///   - GNU ld     (ld --Map=)
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_LLDMAPLOADER_H
#define NEVERD_DEBUG_LLDMAPLOADER_H

#include "neverd/debug/MapDebugContextBase.h"

#include <filesystem>
#include <memory>

namespace neverd {

class LLDMapDebugContext : public MapDebugContextBase {
public:
  static std::unique_ptr<LLDMapDebugContext>
  load(const std::filesystem::path &MapPath, uint64_t ImageBase = 0);
};

} // namespace neverd

#endif // NEVERD_DEBUG_LLDMAPLOADER_H
