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

namespace neverd::source_inline {
inline bool hasUnavailableObservations(const llvm::Function &Function) {
  llvm::SmallVector<const llvm::Instruction *, 32> Work;
  llvm::SmallPtrSet<const llvm::Instruction *, 32> Seen;
  size_t Instructions = limits::pipeline::kSourceInlineBodyOperations;
  for (const auto &Instruction : llvm::instructions(Function)) {
    if (!Instructions)
      return true;
    --Instructions;
    if (llvm_source_unknown::isValue(&Instruction))
      continue;
    // Dead register PHIs can retain unknown inputs after mem2reg. Only a
    // path to a reader, effect or control decision makes them observations.
    // Memory reads and all other calls remain conservative, even if LLVM
    // could prove their results dead with additional analysis.
    if (Instruction.isTerminator() || Instruction.mayReadOrWriteMemory() ||
        Instruction.mayHaveSideEffects() ||
        llvm::isa<llvm::CallBase>(Instruction))
      if (Seen.insert(&Instruction).second)
        Work.push_back(&Instruction);
  }
  size_t Remaining = limits::pipeline::kSourceInlineExpansionOperations;
  while (!Work.empty()) {
    const auto *Instruction = Work.pop_back_val();
    if (llvm_source_unknown::isValue(Instruction))
      return true;
    for (const auto &Operand : Instruction->operands()) {
      if (!Remaining)
        return true;
      --Remaining;
      const auto *Input = llvm::dyn_cast<llvm::Instruction>(Operand.get());
      if (Input && Seen.insert(Input).second)
        Work.push_back(Input);
    }
  }
  return false;
}

} // namespace neverd::source_inline

#endif // NEVERD_PIPELINE_SOURCECALLOBSERVATIONS_H
