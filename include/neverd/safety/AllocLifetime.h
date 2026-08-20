//===- AllocLifetime.h - Heap allocation lifetime audit ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tracks each heap handle across a function's control-flow graph and reports
/// the three lifetime defects: a handle that is never released and never
/// escapes (leak), a handle released twice on a path (double free), and a
/// handle used after it is released (use after free).  Allocation and release
/// wrappers are recognised through per-function escape summaries so an
/// indirection such as a `malloc`/`free` forwarder does not hide the defect.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_ALLOCLIFETIME_H
#define NEVERD_SAFETY_ALLOCLIFETIME_H

#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SafetyTypes.h"
#include "neverd/safety/SinkCatalog.h"

#include <vector>

namespace neverd::safety {

/// Run the heap-lifetime audit over every lifted function and return one
/// finding per detected defect.
std::vector<Finding> auditHeap(const AnalysisInput &In, const SinkCatalog &Cat,
                               const SafetyBudgets &Budgets);

} // namespace neverd::safety

#endif // NEVERD_SAFETY_ALLOCLIFETIME_H
