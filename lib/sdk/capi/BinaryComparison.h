//===- BinaryComparison.h - Native comparison snapshots --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapts the SDK's shared decoded instruction traversal to the comparison
/// model. The snapshot owns its data and never borrows a live session.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_BINARYCOMPARISON_H
#define NEVERD_SDK_CAPI_BINARYCOMPARISON_H

#include "neverd/diff/Comparison.h"
#include "neverd/diff/Program.h"

#include <map>
#include <optional>
#include <string>

namespace neverd::sdk {
struct Session;

struct ComparisonInputOptions {
  std::string Profile = limits::diff::kDefaultNativeProfile;
  bool Discover = true;
  uint64_t MaxInstructions = limits::diff::kDefaultMaxInstructions;
  uint64_t MaxFunctions = limits::diff::kDefaultMaxFunctions;
  uint64_t MaxMemoryBytes = limits::diff::kDefaultMaxMemoryBytes;
  std::optional<uint64_t> Begin;
  std::optional<uint64_t> End;
  std::string Function;
  diff::DataFlowSettings DataFlow;
};

struct ComparisonInput {
  diff::Program Program;
  /// Owned presentation names, excluded from matching and snapshot identity.
  std::map<uint64_t, std::string> DisplayNames;
  std::optional<diff::ComparisonState> Stopped;
  std::string StoppedPhase;
};

std::optional<ComparisonInput>
readComparisonInput(Session &S, const ComparisonInputOptions &Options,
                    const diff::ComparisonControl &Control, std::string &Error);
std::shared_ptr<const diff::DataFlowEvidence>
readComparisonDataFlow(Session &S, const diff::Program &Program,
                       const diff::DataFlowSettings &Settings,
                       const diff::ComparisonControl &Control);
} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_BINARYCOMPARISON_H
