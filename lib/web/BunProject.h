//===- BunProject.h - Derived Bun reconstruction projects --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Development reconstruction export, separate from immutable evidence export.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PRIVATE_BUNPROJECT_H
#define NEVERD_WEB_PRIVATE_BUNPROJECT_H

#include "neverd/web/Bun.h"

namespace neverd::web {

struct BunProjectLimits {
  static constexpr uint64_t HardRecords = 512ULL * 1024;
  static constexpr uint64_t HardUTF16Units = 16ULL * 1024 * 1024;
  // Project-wide metadata admission, independent of per-source parser limits
  // and the writer's byte/file limits. Callers may only lower these ceilings.
  uint64_t Records = HardRecords, UTF16Units = HardUTF16Units;
};

/// Consumes an admitted extraction. Writes fixed local names exclusively;
/// source syntax relocations are verified, runtime equivalence is not claimed.
/// No original executable or cached bytecode is copied into build inputs.
std::string exportBunProject(const BunExtraction &Extraction,
                             std::string_view OutputDirectory,
                             BunProjectLimits Limits = {});
} // namespace neverd::web

#endif // NEVERD_WEB_PRIVATE_BUNPROJECT_H
