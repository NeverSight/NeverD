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

#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"

#include <functional>

namespace neverd {

bool analyzeVoidReturn(const LLVMCAnalysisState &State, llvm::Function &Fn) {
  auto *RetTy = Fn.getReturnType();
  if (RetTy->isVoidTy())
    return true;
  if (!RetTy->isIntegerTy())
    return false;

  bool AllRetResidual = true;
  bool HasComputation = false;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&Inst)) {
        auto *RV = RI->getReturnValue();
        if (!RV)
          continue;
        if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RV)) {
          if (CI->isZero())
            continue;
        }
        if (State.ForwardedLoads.count(RV)) {
          AllRetResidual = false;
          continue;
        }
        const llvm::Value *Source = RV;
        while (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Source))
          Source = Cast->getOperand(0);
        const auto *CallProducer = llvm::dyn_cast<llvm::CallInst>(Source);
        if (!CallProducer)
          if (const auto *Extract =
                  llvm::dyn_cast<llvm::ExtractValueInst>(Source))
            CallProducer =
                llvm::dyn_cast<llvm::CallInst>(Extract->getAggregateOperand());
        if (CallProducer) {
          if (llvm_value_provenance::isSemanticProducer(*CallProducer))
            AllRetResidual = false;
          continue;
        }
        AllRetResidual = false;
      }
      if (Inst.isBinaryOp() || llvm::isa<llvm::ICmpInst>(&Inst) ||
          llvm::isa<llvm::FCmpInst>(&Inst) ||
          llvm::isa<llvm::SelectInst>(&Inst))
        if (!State.DeadFrameStores.count(&Inst) &&
            !State.Inlinable.count(&Inst))
          HasComputation = true;
    }
  }
  return AllRetResidual && !HasComputation;
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
    if (llvm::isa<llvm::CallInst>(Inst))
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
