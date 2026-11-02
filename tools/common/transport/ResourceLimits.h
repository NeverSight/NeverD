//===- ResourceLimits.h - Resource limits and defaults -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_COMMON_TRANSPORT_RESOURCELIMITS_H
#define NEVERD_TOOLS_COMMON_TRANSPORT_RESOURCELIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::transport {

// Classic listing protocol: the maximum rows on either side of an anchor.
inline constexpr std::size_t kListingMaxPageLines = 2000;

// tools/common/transport/Json.h
inline constexpr std::size_t kJsonMaxJsonBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kJsonMax = 4096;

// tools/common/transport/Json.cpp
inline constexpr int kJsonParseJsonDepthLimit = 64;
inline constexpr std::size_t kJsonRetainedParserBytes = 64 * 1024;

} // namespace neverd::limits::transport

#endif // NEVERD_TOOLS_COMMON_TRANSPORT_RESOURCELIMITS_H
