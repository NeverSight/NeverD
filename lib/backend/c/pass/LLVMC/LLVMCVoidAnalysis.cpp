//===- LLVMCVoidAnalysis.cpp - Void return analysis -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Void-return inference and dead-chain propagation for the LLVM-route
/// C emitter.
///
//===----------------------------------------------------------------------===//

#include "LLVMCFrameAliases.h"

#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"

#include <functional>

namespace neverd {

bool analyzeVoidReturn(const LLVMCAnalysisState &State, llvm::Function &Fn) {
  // Whether the function returns a value is settled on MedIR, for both C
  // backends (settleReturnContracts): this IR folds the undefined register a
  // function hands back to the same constant as a deliberate zero, and a
  // callee's result handed back is a value unless that callee returns none.
  (void)State;
  return Fn.getReturnType()->isVoidTy() ||
         llvm_value_provenance::returnsNoValue(Fn);
}

void analyzeVoidDeadChain(LLVMCAnalysisState &State, llvm::Function &Fn) {
  std::set<const llvm::Value *> VoidDead;
  std::function<void(const llvm::Value *)> Collect;
  Collect = [&](const llvm::Value *V) {
    if (!V || VoidDead.count(V))
      return;
    auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
    if (!Inst)
      return;
    // Discarding a return carrier does not discard memory effects or control
    // flow. Share the same eligibility gate as ordinary dead-value cleanup.
    if (!llvmc::canDropDeadFrameValue(*Inst))
      return;
    bool AllDead = true;
    for (auto *U : Inst->users()) {
      if (llvm::isa<llvm::ReturnInst>(U))
        continue;
      if (VoidDead.count(U))
        continue;
      if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U))
        if (State.DeadFrameStores.count(UI))
          continue;
      AllDead = false;
      break;
    }
    if (AllDead) {
      VoidDead.insert(V);
      for (unsigned I = 0; I < Inst->getNumOperands(); ++I)
        Collect(Inst->getOperand(I));
    }
  };
  for (auto &BB : Fn)
    for (auto &Inst : BB)
      if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&Inst))
        if (RI->getReturnValue())
          Collect(RI->getReturnValue());
  for (auto *V : VoidDead)
    State.DeadFrameStores.insert(llvm::cast<llvm::Instruction>(V));
}

} // namespace neverd
