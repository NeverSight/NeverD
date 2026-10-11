//===- BunSourceMap.h - Bun serialized source maps ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bun serialized source maps.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Bun.h"
#include "neverd/web/SourceMap.h"

namespace neverd::web {
inline constexpr std::string_view BunSourceMapProfile =
    "bun-1.4.2-serialized-source-map-v1";
inline constexpr uint64_t MaxBunMapSources = 10000;
inline constexpr uint64_t MaxBunMapSourceBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxBunMapDecodedBytes = 8 * 1024 * 1024;

/// Requires in-process LLVM Zstd support; never invokes an external decoder.
bool bunSourceMapAvailable();
/// Decode an already extracted module's map, retaining its raw storage links.
/// Malformed structure, unsupported frames and budget failures publish nothing.
SourceMap decodeBunSourceMap(const BunExtraction &Extraction,
                             const BunModule &Module);
} // namespace neverd::web
