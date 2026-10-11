//===- HTMLImportMaps.h - Captured import map contexts -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured import map contexts.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/HTML.h"

namespace neverd::web {
struct HTMLImportMapSelection {
  std::string Status = "not_analyzed", BaseURL;
  std::vector<const ImportMap *> Maps;
};
HTMLImportMapSelection selectHTMLImportMaps(const HTMLDocument &Document,
                                            const HTMLImportMaps &Maps,
                                            uint32_t Script,
                                            const ImportMapCharge &Charge);

/// Resolve a private URL candidate and separately project it into the local
/// capture namespace. A full URL identity is never a filesystem lookup key.
HTMLLocalURL importMapFileCandidate(const ImportMapResolution &Resolution,
                                    const HTMLImportMaps &Maps,
                                    const HTMLLocalURL &ScriptBase,
                                    std::string_view Specifier,
                                    const ImportMapCharge &Charge);
} // namespace neverd::web
