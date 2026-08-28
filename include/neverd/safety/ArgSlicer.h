//===- ArgSlicer.h - Classify a sink argument by its provenance -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A backward walk over the SSA def-use graph of one lifted function that
/// classifies the argument deciding a sink's safety property.  A provably
/// bounded argument (a constant, a length-returning call, a clamp, arithmetic
/// on bounded values) lets the caller retire the sink without symbolic
/// execution; an argument that reaches an external input is flagged for
/// exploration; anything unresolved stays UNKNOWN so the sink is never assumed
/// safe.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_ARGSLICER_H
#define NEVERD_SAFETY_ARGSLICER_H

#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SafetyTypes.h"
#include "neverd/safety/SinkCatalog.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

struct MedFunc;
struct MedVar;

namespace safety {

/// The outcome of slicing one argument.
struct ArgClassification {
  ArgFlow Flow = ArgFlow::Unknown;
  std::optional<uint64_t> ConstValue; ///< set when the argument is a constant.
  /// Inclusive numeric upper bound proved by the slice.  `Bounded` without a
  /// concrete upper bound is not sufficient to retire a sink: the caller must
  /// still compare this value with the recovered destination capacity.
  std::optional<uint64_t> UpperBound;
  /// True when retiring the sink from the slice alone would bypass a call on
  /// which this value depends.  The caller must validate the full path before
  /// reporting SAFE, because the call may not have a symbolic summary.
  bool RequiresPathValidation = false;
  std::string Reason;      ///< why it was classed as it was.
  std::string TaintSource; ///< the reaching input, when TAINTED.
};

/// Classify the argument at position \p ArgIndex of the call record
/// \p CallInfoIndex in \p F.  \p In and \p Cat supply callee identity and the
/// source/bounded-return vocabulary.
ArgClassification classifyArgument(const AnalysisInput &In,
                                   const SinkCatalog &Cat, const MedFunc &F,
                                   size_t CallInfoIndex, int ArgIndex);

} // namespace safety
} // namespace neverd

#endif // NEVERD_SAFETY_ARGSLICER_H
