//===- CallAlignment.h - Unambiguous call-sequence anchors -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Call positions shared by every longest alignment of already corresponding
/// callees. Unresolved positions do not supply anchors.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DIFF_CALLALIGNMENT_H
#define NEVERD_LIB_DIFF_CALLALIGNMENT_H

#include "neverd/diff/Control.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::diff {

/// Both sequences use the same callee identities; NoIndex supplies no anchor.
/// Returns original position pairs, or nullopt on cancellation/deadline.
std::optional<std::vector<std::pair<uint32_t, uint32_t>>>
uniqueCallAnchors(llvm::ArrayRef<uint32_t> Primary,
                  llvm::ArrayRef<uint32_t> Secondary,
                  const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_CALLALIGNMENT_H
