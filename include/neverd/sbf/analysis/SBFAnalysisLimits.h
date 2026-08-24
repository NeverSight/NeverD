//===- SBFAnalysisLimits.h - Typed SBF analysis resource policy -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exposes the optional-analysis resource limits from the single typed
/// definition authority. These are host-work limits, not protocol or verifier
/// limits.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFANALYSISLIMITS_H
#define NEVERD_SBF_ANALYSIS_SBFANALYSISLIMITS_H

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace neverd::sbf {

#define SBF_ANALYSIS_LIMIT(NAME, VALUE) inline constexpr size_t k##NAME = VALUE;
#define SBF_ANALYSIS_DIAGNOSTIC(NAME, TEXT)                                    \
  inline constexpr llvm::StringLiteral k##NAME(TEXT);
#include "neverd/sbf/analysis/SBFAnalysisLimits.def"

/// Return the exact edge capacity remaining after call-graph nodes, or no
/// value when even the node set exceeds a typed public-output budget. The
/// subtraction form prevents hostile counts from overflowing a total.
[[nodiscard]] constexpr std::optional<size_t>
callGraphEdgeCapacity(size_t NodeCount) {
  if (NodeCount > kCallGraphOutputNodeBudget ||
      NodeCount > kCallGraphOutputElementBudget)
    return std::nullopt;
  return std::min(kCallGraphOutputEdgeBudget,
                  kCallGraphOutputElementBudget - NodeCount);
}

/// Overflow-safe counter for a streamed optional-analysis artifact. Consumers
/// can run their canonical serializer against this counter before allocating
/// the real result, making encoded-byte limits exact rather than estimates of
/// container payload.
class AnalysisOutputByteBudget {
public:
  explicit constexpr AnalysisOutputByteBudget(size_t Limit) : Limit(Limit) {}

  [[nodiscard]] constexpr bool consume(size_t ByteCount) {
    if (ByteCount > Limit - Consumed) {
      Exceeded = true;
      return false;
    }
    Consumed += ByteCount;
    return true;
  }

  [[nodiscard]] constexpr size_t consumed() const { return Consumed; }
  [[nodiscard]] constexpr bool exceeded() const { return Exceeded; }

private:
  size_t Limit = 0;
  size_t Consumed = 0;
  bool Exceeded = false;
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFANALYSISLIMITS_H
