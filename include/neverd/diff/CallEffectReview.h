//===- CallEffectReview.h - Qualified call-site review ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Applicability-checked external-call contracts on observed native paths.
/// A contract describes a call, not the safety or feasibility of a path. It
/// never closes the graph's memory, lifetime, return or parameter boundaries.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_CALLEFFECTREVIEW_H
#define NEVERD_DIFF_CALLEFFECTREVIEW_H

#include "neverd/diff/CallImpact.h"
#include "neverd/diff/DataFlow.h"
#include "neverd/safety/CallEffects.h"

namespace neverd::diff {

inline constexpr llvm::StringLiteral CallEffectReviewProfile =
    "native-import-effects-v1";

enum class CallEffectStatus : uint8_t {
  Qualified,
  UnresolvedTarget,
  InternalTarget,
  UnqualifiedIdentity,
  UnsupportedEnvironment,
  SSAUnavailable,
  MissingCall,
  AmbiguousCall,
  MissingArguments,
  UnsupportedContract,
};
llvm::StringRef callEffectStatusName(CallEffectStatus Status);
llvm::StringRef callEffectFamilyName(safety::CallEffectFamily Family);
std::vector<llvm::StringRef>
callEffectCapabilityNames(safety::CallEffectCapability Capabilities);

struct CallEffectReviewOptions {
  uint32_t MaxRecords = limits::diff::kDefaultEffectRecords;
  uint64_t MaxWork = limits::diff::kDefaultEffectWork;
  DataFlowOptions Graph;
  bool operator==(const CallEffectReviewOptions &) const = default;
  std::optional<std::string> validate() const;
};

struct CallEffectObservation {
  /// Site in the supplied native CallImpact; original bytes are never copied
  /// from or attributed to a synthetic SSA operation.
  uint32_t Site = NoIndex;
  uint32_t Node = NoIndex;
  CallEffectStatus Status = CallEffectStatus::UnresolvedTarget;
  uint32_t ArgumentCount = 0;
  uint32_t SSAGaps = 0;
  safety::CallEffectFamily Family = safety::CallEffectFamily::None;
  safety::CallEffectCapability Capabilities =
      safety::CallEffectCapability::NoEffect;
  /// Catalog roles are populated only when the exact summary supports that
  /// catalog entry. They are positions in the recovered argument span.
  int Destination = -1, Source = -1, Length = -1, Capacity = -1, Format = -1;
  int Handle = -1, InputBuffer = -1;
  bool ReleaseMayFail = false, UnboundedWrite = false;
  bool operator==(const CallEffectObservation &) const = default;
};

struct CallEffectReview {
  ComparisonState State = ComparisonState::Complete;
  bool Complete = false;
  uint64_t Work = 0, VisitedSites = 0, Qualified = 0;
  std::vector<CallEffectObservation> Records;
};

/// Review every indexed transfer in a reached caller, including unresolved
/// sites and edges beyond the traversal frontier. Scope is exactly the supplied
/// native analysis. It must be freshly derived from P; its indices are checked.
/// Only an exact bodiless native import and a unique native-origin SSA call
/// with recovered arguments may acquire a Safety call contract. Symbol/debug
/// names, unresolved targets, thunks and source-name resemblance never do.
/// P's retained graphs remain immutable. No interprocedural data propagation,
/// alias, capacity, must-release or authorization claim is inferred.
CallEffectReview reviewCallEffects(const Program &P, const CallImpact &Impact,
                                   const CallEffectReviewOptions &Options = {},
                                   const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_CALLEFFECTREVIEW_H
