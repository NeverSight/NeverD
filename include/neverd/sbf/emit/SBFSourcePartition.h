//===- SBFSourcePartition.h - Bounded source regions ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Partitions source rendering under the canonical HighIR owner authority.
/// Shared and unowned slots remain explicit. This is a mechanical source
/// layout, not a new function-discovery or control-flow interpretation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_EMIT_SBFSOURCEPARTITION_H
#define NEVERD_SBF_EMIT_SBFSOURCEPARTITION_H

#include "neverd/sbf/SBFIR.h"
#include "neverd/sbf/emit/SBFSourceLimits.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {

enum class SourceRegionOwner : uint8_t { Function, Shared, Unowned };

struct SourcePartitionRegion {
  SourceRegionOwner Owner = SourceRegionOwner::Unowned;
  std::optional<size_t> FunctionIndex;
  std::string Name;
  std::vector<size_t> Slots;
};

struct SourcePartition {
  std::vector<SourcePartitionRegion> Regions;
  std::vector<size_t> RegionBySlot;
};

/// Every raw slot, including continuations and unreachable code, belongs to
/// exactly one rendering region. The source VM state crosses region boundaries
/// unchanged; only instruction execution may alter it.
llvm::Expected<SourcePartition>
partitionSource(const SBFProgram &Program,
                size_t MaximumSlots = kMaximumDispatcherRegionSlots);

} // namespace neverd::sbf

#endif // NEVERD_SBF_EMIT_SBFSOURCEPARTITION_H
