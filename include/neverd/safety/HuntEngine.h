//===- HuntEngine.h - Dangerous-copy overflow hunt --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decides whether a copy sink can write past its destination.  A fortified or
/// provably bounded copy is retired without a solver; a constant length is
/// compared to the recovered capacity.  Otherwise the engine walks the LowIR
/// CFG toward the sink, prunes infeasible branches with the bitvector solver,
/// and asks whether any reachable path predicate together with
/// \c copy_len > capacity is satisfiable.  A model of that query is retained
/// as symbolic evidence; it is marked replayable only after an external-input
/// adapter maps the model back to process bytes.  A destination whose capacity
/// is unknown, a sink the
/// pipeline could not fully recover, or an exhausted budget stays UNKNOWN —
/// never SAFE.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_HUNTENGINE_H
#define NEVERD_SAFETY_HUNTENGINE_H

#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SafetyTypes.h"
#include "neverd/safety/SinkCatalog.h"

#include <optional>

namespace neverd {

struct MedFunc;

namespace safety {

/// Evaluate one copy sink and, when it is a copy the hunt understands, return a
/// finding.  Non-copy sinks return no finding (the audit track owns them).
std::optional<Finding> huntSink(const AnalysisInput &In, const SinkCatalog &Cat,
                                const SafetyBudgets &Budgets, const MedFunc &F,
                                const SinkSite &Site);

} // namespace safety
} // namespace neverd

#endif // NEVERD_SAFETY_HUNTENGINE_H
