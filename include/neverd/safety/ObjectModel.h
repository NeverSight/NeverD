//===- ObjectModel.h - Destination capacity and heap-object sizing *- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers the capacity of the destination a copy writes into, so the hunt
/// engine can decide whether a copy length can exceed it.  Capacity is taken,
/// in order, from a debug-declared array type, then a heap allocation site with
/// a known size, then a sound stack-frame upper bound; when none applies the
/// capacity is left unknown and the sink is never assumed safe.  Every reported
/// capacity is an upper bound on the true object size, so a proven violation is
/// never a false positive.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_OBJECTMODEL_H
#define NEVERD_SAFETY_OBJECTMODEL_H

#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SinkCatalog.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

struct MedFunc;
struct MedVar;

namespace safety {

/// Where a destination pointer lives.
enum class ObjectRegion { Stack, Heap, Global, Unknown };

/// The recovered description of a destination buffer.
struct DestObject {
  ObjectRegion Region = ObjectRegion::Unknown;
  /// Upper bound on the object's byte capacity, when recoverable.
  std::optional<uint64_t> Capacity;
  /// True only when Capacity is the object's recovered size rather than a
  /// containing-frame upper bound.
  bool CapacityExact = false;
  int64_t StackOffset = 0; ///< signed offset from the incoming stack pointer.
  std::string Detail;      ///< where the capacity came from.
};

/// Resolve the destination buffer of a copy: \p DstArgIndex is the destination
/// argument position of the call record \p CallInfoIndex in \p F.
DestObject resolveDestination(const AnalysisInput &In, const SinkCatalog &Cat,
                              const MedFunc &F, size_t CallInfoIndex,
                              int DstArgIndex);

} // namespace safety
} // namespace neverd

#endif // NEVERD_SAFETY_OBJECTMODEL_H
