//===- DeadStoreAnalysis.cpp - Dead frame-store analysis --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead frame-store analysis for the LLVM-route C emitter.  Identifies
/// byte-array allocas whose stored values are never loaded.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"

#include <functional>

namespace neverd {

namespace {

bool ptrDerivesFrom(const llvm::Value *V, const llvm::AllocaInst *AI,
                    std::set<const llvm::Value *> &Visited) {
  if (V == AI)
    return true;
  if (!Visited.insert(V).second)
    return false;
  if (auto *Inst = llvm::dyn_cast<llvm::Instruction>(V)) {
    for (unsigned I = 0; I < Inst->getNumOperands(); ++I)
      if (ptrDerivesFrom(Inst->getOperand(I), AI, Visited))
        return true;
  }
  return false;
}

} // anonymous namespace

void analyzeDeadFrameStores(LLVMCAnalysisState &State, llvm::Function &Fn) {
  State.DeadFrameAllocas.clear();
  State.DeadFrameStores.clear();

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!AI)
        continue;
      auto *Arr = llvm::dyn_cast<llvm::ArrayType>(AI->getAllocatedType());
      if (!Arr || !Arr->getElementType()->isIntegerTy(8))
        continue;

      bool HasLoad = false;
      std::set<const llvm::Value *> Visited;
      std::function<void(const llvm::Value *)> Check =
          [&](const llvm::Value *V) {
            if (HasLoad || !Visited.insert(V).second)
              return;
            for (auto *User : V->users()) {
              if (llvm::isa<llvm::LoadInst>(User)) {
                HasLoad = true;
                return;
              }
              if (auto *CI = llvm::dyn_cast<llvm::CallInst>(User)) {
                for (unsigned I = 0; I < CI->arg_size(); ++I)
                  if (CI->getArgOperand(I) == V) {
                    HasLoad = true;
                    return;
                  }
              }
              if (!llvm::isa<llvm::StoreInst>(User))
                Check(User);
            }
          };
      Check(AI);
      if (!HasLoad)
        State.DeadFrameAllocas.insert(AI);
    }
  }

  if (State.DeadFrameAllocas.empty())
    return;

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      for (auto *AI : State.DeadFrameAllocas) {
        std::set<const llvm::Value *> Visited;
        if (ptrDerivesFrom(SI->getPointerOperand(), AI, Visited)) {
          State.DeadFrameStores.insert(SI);
          break;
        }
      }
    }
  }

  std::set<const llvm::Value *> DeadCheckVisited;
  std::function<bool(const llvm::Value *)> AllUsersDead =
      [&](const llvm::Value *V) -> bool {
    if (!DeadCheckVisited.insert(V).second)
      return true;
    for (auto *User : V->users()) {
      auto *UI = llvm::dyn_cast<llvm::Instruction>(User);
      if (!UI)
        return false;
      if (State.DeadFrameStores.count(UI))
        continue;
      if (State.Inlinable.count(UI) && AllUsersDead(UI))
        continue;
      return false;
    }
    return true;
  };

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (Inst.getType()->isVoidTy())
          continue;
        if (llvm::isa<llvm::AllocaInst>(&Inst))
          continue;
        if (llvm::isa<llvm::CallInst>(&Inst))
          continue;
        if (State.DeadFrameStores.count(&Inst))
          continue;
        if (Inst.use_empty())
          continue;
        DeadCheckVisited.clear();
        if (AllUsersDead(&Inst)) {
          State.DeadFrameStores.insert(&Inst);
          Changed = true;
        }
      }
    }
  }
}

} // namespace neverd
