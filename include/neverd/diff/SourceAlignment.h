//===- SourceAlignment.h - Exact source-line correspondence ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Textual source alignment, independent of native matching and proof results.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_SOURCEALIGNMENT_H
#define NEVERD_DIFF_SOURCEALIGNMENT_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <vector>

namespace neverd::diff {

inline constexpr uint64_t NoSourceLine = UINT64_MAX;

enum class SourceLineKind : uint8_t { Unchanged, Changed, Inserted, Deleted };
llvm::StringRef sourceLineKindName(SourceLineKind Kind);

/// Indices refer to the caller's exact source lines. Changed rows pair adjacent
/// deletions and insertions for display; they do not establish statement or
/// instruction correspondence. Every input line occurs exactly once on success.
struct SourceLinePair {
  uint64_t Primary = NoSourceLine, Secondary = NoSourceLine;
  SourceLineKind Kind = SourceLineKind::Unchanged;
  bool operator==(const SourceLinePair &) const = default;
};

struct SourceAlignmentOptions {
  /// Charges input bytes, frontier storage/search, comparisons and output rows.
  /// Zero removes the extra work allowance; cancellation remains available.
  uint64_t MaxWork = limits::diff::kDefaultSourceAlignmentWork;
};

struct SourceAlignment {
  ComparisonState State = ComparisonState::Complete;
  bool ComputationComplete = false;
  uint64_t Work = 0;
  std::vector<SourceLinePair> Rows;
};

/// Exact-line longest-common-subsequence alignment with linear scratch space.
/// Bidirectional edit-path search makes sparse edits output-sensitive, while
/// prefix/suffix stripping preserves long unchanged runs. Ties are
/// deterministic but do not imply unique source correspondence. Stopped work
/// publishes no classified rows. Neither spelling equality nor this alignment
/// proves behavior.
SourceAlignment alignSourceLines(llvm::ArrayRef<llvm::StringRef> Primary,
                                 llvm::ArrayRef<llvm::StringRef> Secondary,
                                 const SourceAlignmentOptions &Options = {},
                                 const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_SOURCEALIGNMENT_H
