//===- Config.h - Worker presentation defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Listing columns, string previews and graph layout defaults.
/// Request ceilings are defined in ResourceLimits.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_CONFIG_H
#define NEVERD_TOOLS_NEVERD_WORKER_CONFIG_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::worker_config {

// tools/neverd-worker/GraphLayout.cpp
inline constexpr double kGraphLayoutPortSpacing = 12;

// tools/neverd-worker/Listing.cpp
// Listing layout, in characters after the prefix: name, mnemonic and comment
// columns of a conventional interactive disassembler listing.
inline constexpr std::size_t kListingNameColumnWidth = 16;
inline constexpr std::size_t kListingMnemonicWidth = 8;
inline constexpr std::size_t kListingCommentColumn = 40;
// Automatic string names keep this many characters, the `a` included.
inline constexpr std::size_t kListingStringNameLength = 15;
// A string a comment quotes shows this many escaped characters.
inline constexpr std::size_t kListingCommentStringLength = 39;
// A two-level switch's index table lists this many entries a line, as a
// conventional interactive disassembler groups them.
inline constexpr std::size_t kListingSwitchIndexEntriesPerLine = 4;

} // namespace neverd::limits::worker_config

#endif // NEVERD_TOOLS_NEVERD_WORKER_CONFIG_H
