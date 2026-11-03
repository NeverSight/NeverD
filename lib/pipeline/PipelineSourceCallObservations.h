//===- PipelineSourceCallObservations.h - Complete body inputs --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound the dependency graph of observable inputs without deleting effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_SOURCECALLOBSERVATIONS_H
#define NEVERD_PIPELINE_SOURCECALLOBSERVATIONS_H

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/pipeline/Limits.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstIterator.h"

#include <cstddef>
#include <optional>

namespace neverd::source_inline {

struct BodyObservations {
  size_t Instructions = 0;
  bool Unavailable = false;
};

namespace detail {
struct ObservationGraph {
  llvm::SmallPtrSet<const llvm::Instruction *, 32> Live;
  bool Unavailable = false;
};

inline std::optional<ObservationGraph>
observeBody(const llvm::Function &Function) {
  ObservationGraph Graph;
  llvm::SmallVector<const llvm::Instruction *, 32> Work;
  size_t Remaining = limits::pipeline::kSourceInlineExpansionOperations;
  for (const auto &Instruction : llvm::instructions(Function)) {
    if (!Remaining)
      return std::nullopt;
    --Remaining;
    if (llvm_source_unknown::isValue(&Instruction))
      continue;
    // Every memory read remains an observation, including an unused load.
    // Unknown providers are analysis scaffolding, with no runtime effects.
    // They become observations only when an effect or decision uses them.
    if (Instruction.isTerminator() || Instruction.mayReadOrWriteMemory() ||
        Instruction.mayHaveSideEffects() ||
        llvm::isa<llvm::CallBase>(Instruction))
      if (Graph.Live.insert(&Instruction).second)
        Work.push_back(&Instruction);
  }
  while (!Work.empty()) {
    const auto *Instruction = Work.pop_back_val();
    Graph.Unavailable |= llvm_source_unknown::isValue(Instruction);
    for (const auto &Operand : Instruction->operands()) {
      if (!Remaining)
        return std::nullopt;
      --Remaining;
      const auto *Input = llvm::dyn_cast<llvm::Instruction>(Operand.get());
      if (Input && Graph.Live.insert(Input).second)
        Work.push_back(Input);
    }
  }
  if (Graph.Live.size() > limits::pipeline::kSourceInlineBodyOperations)
    return std::nullopt;
  return Graph;
}
} // namespace detail

inline bool hasUnavailableObservations(const llvm::Function &Function) {
  const auto Graph = detail::observeBody(Function);
  return !Graph || Graph->Unavailable;
}

/// Compact only the request-local source body, never a native input proof.
/// Count its observable graph against the expansion limit after dropping dead
/// register/flag calculations and unused analysis-only unknown providers.
/// No load, store, ordinary call, terminator or exceptional effect is removed.
/// Exhausting either bound leaves the body untouched.
inline std::optional<BodyObservations>
compactSourceBody(llvm::Function &Function) {
  const auto Graph = detail::observeBody(Function);
  if (!Graph)
    return std::nullopt;
  llvm::SmallVector<llvm::Instruction *, 32> Dead;
  for (auto &Instruction : llvm::instructions(Function))
    if (!Graph->Live.contains(&Instruction))
      Dead.push_back(&Instruction);
  // Dropping references together also handles unused PHI cycles. A live
  // instruction cannot refer to this set: the complete operand closure above
  // would have marked that dependency live.
  for (auto *Instruction : Dead)
    Instruction->dropAllReferences();
  for (auto *Instruction : Dead)
    Instruction->eraseFromParent();
  return BodyObservations{Graph->Live.size(), Graph->Unavailable};
}

} // namespace neverd::source_inline

#endif // NEVERD_PIPELINE_SOURCECALLOBSERVATIONS_H
