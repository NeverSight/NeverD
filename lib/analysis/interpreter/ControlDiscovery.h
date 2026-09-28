//===- ControlDiscovery.h - Bounded control dependency discovery --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H
#define NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/symbolic/SymState.h"

namespace neverd::analysis::detail {

enum class ControlDiscoveryStatus {
  Complete,
  UnsupportedOrigin,
  BudgetExceeded,
};

/// DemandedBits numbers bits in the scalar read of this byte range using the
/// state's byte order. Only those bits influence the discovered expression;
/// the byte range itself remains a candidate, not a finite-value fact.
struct ControlRegisterDemand {
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
  uint64_t DemandedBits = 0;
};

struct ControlFrameDemand {
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  uint64_t DemandedBits = 0;
};

struct ControlDiscovery {
  ControlDiscoveryStatus Status = ControlDiscoveryStatus::Complete;
  /// Candidate locations only: neither finiteness nor an incoming value is
  /// established. UnsupportedOrigin may retain useful candidates discovered
  /// through a load's address; BudgetExceeded always clears the candidates.
  std::vector<ControlRegisterDemand> RegisterRanges;
  std::vector<ControlFrameDemand> FrameSlots;
  /// Charged unique expression/slice work items, inspected origin records,
  /// and 64-bit words processed by constant-mask and multiplier scans.
  uint64_t Visited = 0;
};

/// Exact canonical entry-root identity, not a numeric alias or range proof.
/// The supported frame model has one 64-bit root and a modular displacement.
std::optional<uint64_t> frameRelativeOffset(const symbolic::SymContext &Ctx,
                                            symbolic::SymRef Value,
                                            symbolic::SymRef Root);

/// Gather persistent input locations needed by Value without changing State
/// or its context. Structural and bitwise operations, constant shifts and
/// modular low-prefix arithmetic retain bounded bit demands; other operations
/// conservatively visit complete operands. Only structured
/// entry-register inputs and exact root-relative memory inputs are nominated.
/// Memory input birth is distinct from later loads of equal/forwarded values.
/// Post-clobber inputs and external memory are never nominated as fields,
/// though an external load's address can reveal register/frame dependencies.
ControlDiscovery gatherControlDependencies(const symbolic::SymState &State,
                                           symbolic::SymRef Value,
                                           symbolic::SymRef FrameRoot,
                                           uint64_t MaxVisited);

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H
