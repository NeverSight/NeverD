//===- Safety.h - Memory-safety audit and hunt entry points -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The public entry points for the two memory-safety analyses.  A caller
/// assembles an AnalysisInput from an already lifted session, builds a catalog
/// (optionally extended from specification files), then runs the hunt or audit
/// track and serialises the resulting report to a stable JSON schema.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SAFETY_H
#define NEVERD_SAFETY_SAFETY_H

#include "neverd/safety/AllocLifetime.h"
#include "neverd/safety/ArgSlicer.h"
#include "neverd/safety/HuntEngine.h"
#include "neverd/safety/ObjectModel.h"
#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SafetyTypes.h"
#include "neverd/safety/SinkCatalog.h"
#include "neverd/safety/SinkScanner.h"

#include <string>

namespace neverd::safety {

/// Hunt every copy sink for a destination overflow.
SafetyReport runHunt(const AnalysisInput &In, const SinkCatalog &Cat,
                     const SafetyBudgets &Budgets);

/// Audit every heap handle for a lifetime defect.
SafetyReport runAudit(const AnalysisInput &In, const SinkCatalog &Cat,
                      const SafetyBudgets &Budgets);

/// Serialise a report to the stable JSON schema.  \p Pretty adds indentation.
std::string toJson(const SafetyReport &Report, bool Pretty = false);

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SAFETY_H
