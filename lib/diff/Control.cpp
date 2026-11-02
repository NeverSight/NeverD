//===- Control.cpp - Cooperative comparison operation stops -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared cancellation and deadline precedence for every comparison phase.
//===----------------------------------------------------------------------===//

#include "neverd/diff/Control.h"

namespace neverd::diff {

ComparisonInterrupted::ComparisonInterrupted(ComparisonState State)
    : std::runtime_error("comparison " + stateName(State).str()), State(State) {
}

std::optional<ComparisonState> ComparisonControl::stop() const {
  if (Cancel && Cancel->load(std::memory_order_relaxed))
    return ComparisonState::Cancelled;
  if (Deadline && std::chrono::steady_clock::now() >= *Deadline)
    return ComparisonState::TimedOut;
  return std::nullopt;
}

void ComparisonControl::check() const {
  if (const auto State = stop())
    throw ComparisonInterrupted(*State);
}

void ComparisonControl::report(llvm::StringRef Phase, uint64_t Done,
                               uint64_t Total) const {
  check();
  if (Progress)
    Progress(Phase, Done, Total);
  check();
}

} // namespace neverd::diff
