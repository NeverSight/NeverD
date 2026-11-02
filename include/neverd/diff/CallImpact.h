//===- CallImpact.h - Observed call paths for change review ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded caller/callee reachability over admitted native relations. A path
/// is possible context for review, not a proof of data impact, path
/// feasibility, patch presence or vulnerability elimination. Indirect targets
/// stay unknown.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_CALLIMPACT_H
#define NEVERD_DIFF_CALLIMPACT_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {
inline constexpr llvm::StringLiteral CallImpactProfile = "native-call-paths-v1";
enum ImpactGap : uint32_t {
  ImpactIndirectCall = 1u << 0,
  ImpactIndirectTransfer = 1u << 1,
  ImpactUnresolvedCall = 1u << 2,
  ImpactNativeCoverage = 1u << 3,
  ImpactExceptionalControl = 1u << 4,
  ImpactExternalBody = 1u << 5,
  ImpactDiscovery = 1u << 6,
};
std::vector<llvm::StringRef> impactGapNames(uint32_t Mask);

struct ImpactOptions {
  unsigned MaxFunctions = limits::diff::kDefaultImpactFunctions;
  /// Zero includes only seeds and reports a boundary if further nodes exist.
  unsigned MaxDepth = limits::diff::kDefaultImpactDepth;
  unsigned MaxSites = limits::diff::kDefaultImpactSites;
  uint64_t MaxWork = limits::diff::kDefaultImpactWork;
  bool operator==(const ImpactOptions &) const = default;
  std::optional<std::string> validate() const;
};
enum class ImpactDirection : uint8_t { Callers, Callees };
struct ImpactSite {
  uint32_t Caller = NoIndex, Callee = NoIndex;
  /// Program instruction index. NoIndex is a function-wide coverage boundary.
  uint32_t Instruction = NoIndex;
  /// Nonzero only for an unresolved site or function-wide boundary.
  uint32_t Gaps = 0;
  bool operator==(const ImpactSite &) const = default;
};
struct ImpactFunction {
  uint32_t Function = NoIndex;
  uint32_t Depth = 0;
  /// Row in Functions nearer the seed, and the observed site connecting it.
  /// Both NoIndex for seeds. Other incoming paths remain in TraversedSites.
  uint32_t Predecessor = NoIndex, WitnessSite = NoIndex;
  uint32_t Gaps = 0;
  bool ScanComplete = false;
  bool operator==(const ImpactFunction &) const = default;
};
struct CallImpact {
  ComparisonState State = ComparisonState::Complete;
  ComparisonState IndexState = ComparisonState::Complete;
  ComparisonState TraversalState = ComparisonState::Complete;
  bool IndexComplete = false, TraversalComplete = false;
  uint64_t Work = 0, IndexedInstructions = 0;
  uint32_t IndexedFunctions = 0, Gaps = 0;
  std::vector<ImpactSite> Sites;
  /// All unresolved sites in the scanned scope, including functions with no
  /// discovered path to a seed. Missing indirect callers cannot be ruled out.
  std::vector<uint32_t> BoundarySites;
  /// Breadth-first discovery order. A function with no caller is not thereby
  /// an entry point; callers must inspect the Program's explicit entry traits.
  std::vector<ImpactFunction> Functions;
  std::vector<uint32_t> TraversedSites;
  /// Known edges reaching a function omitted by the depth/function allowance.
  std::vector<uint32_t> FrontierSites;
  bool operator==(const CallImpact &) const = default;
};

/// Seeds are native function indices, sorted/deduplicated internally. Only
/// Instruction::Callee establishes edges (including admitted tail/import
/// transfers); neither opcode spelling nor an address-like scalar does so.
/// Witness ties use stable reverse instruction order. Completed observations
/// survive cooperative cancellation and limits; partial scope stays explicit.
CallImpact analyzeCallImpact(const Program &P, llvm::ArrayRef<uint32_t> Seeds,
                             ImpactDirection Direction,
                             const ImpactOptions &Options = {},
                             const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_CALLIMPACT_H
