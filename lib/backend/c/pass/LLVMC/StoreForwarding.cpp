//===- LLVMCStoreForwarding.cpp - Store-to-load forwarding -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Store-to-load forwarding analysis for the LLVM-route C emitter.
/// Identifies frame-local stores whose values can be substituted directly
/// at the load site, eliminating redundant memory traffic.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Operator.h"

#include <functional>
#include <optional>
#include <vector>

namespace neverd {

void analyzeStoreForwarding(LLVMCAnalysisState &State, llvm::Function &Fn) {
  State.ForwardedLoads.clear();

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!AI)
        continue;
      if (State.DeadFrameAllocas.count(AI))
        continue;

      auto *Arr = llvm::dyn_cast<llvm::ArrayType>(AI->getAllocatedType());
      if (!Arr || !Arr->getElementType()->isIntegerTy(8))
        continue;

      const llvm::Value *RspInit = nullptr;
      const llvm::Value *RspInitNarrow = nullptr;
      for (auto *User : AI->users()) {
        auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(User);
        if (!GEP)
          continue;
        for (auto *GU : GEP->users()) {
          if (llvm::isa<llvm::PtrToIntInst>(GU)) {
            RspInit = GU;
            for (auto *TU : GU->users()) {
              if (llvm::isa<llvm::TruncInst>(TU))
                RspInitNarrow = TU;
            }
            break;
          }
        }
        if (RspInit)
          break;
      }
      if (!RspInit)
        continue;

      auto UnwrapZext = [](const llvm::Value *V) -> const llvm::Value * {
        if (auto *ZE = llvm::dyn_cast<llvm::ZExtInst>(V))
          return ZE->getOperand(0);
        return V;
      };

      auto GetOffset = [&](const llvm::Value *Ptr) -> std::optional<int64_t> {
        auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Ptr);
        if (!I2P)
          return std::nullopt;
        auto *Inner = UnwrapZext(I2P->getOperand(0));
        auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(Inner);
        if (!Add || Add->getOpcode() != llvm::Instruction::Add)
          return std::nullopt;
        if (Add->getOperand(0) != RspInit &&
            Add->getOperand(0) != RspInitNarrow)
          return std::nullopt;
        auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Add->getOperand(1));
        if (!CI)
          return std::nullopt;
        return CI->getSExtValue();
      };

      std::map<int64_t, const llvm::Value *> OffsetStore;
      std::vector<std::pair<const llvm::LoadInst *, const llvm::Value *>> Fwds;
      std::set<const llvm::StoreInst *> FrameStores;
      bool HasUnfwdLoad = false;

      for (auto &BB2 : Fn) {
        for (auto &Inst2 : BB2) {
          if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst2)) {
            auto Off = GetOffset(SI->getPointerOperand());
            if (Off.has_value()) {
              OffsetStore[*Off] = SI->getValueOperand();
              FrameStores.insert(SI);
            }
          }
          if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst2)) {
            auto Off = GetOffset(LI->getPointerOperand());
            if (Off.has_value()) {
              auto It = OffsetStore.find(*Off);
              if (It != OffsetStore.end())
                Fwds.emplace_back(LI, It->second);
              else
                HasUnfwdLoad = true;
            }
          }
        }
      }

      bool FrameEscapes = false;
      std::set<const llvm::Value *> Checked;
      std::function<void(const llvm::Value *)> CheckEscape =
          [&](const llvm::Value *V) {
            if (FrameEscapes || !Checked.insert(V).second)
              return;
            for (auto *User : V->users()) {
              if (auto *CI = llvm::dyn_cast<llvm::CallInst>(User)) {
                if (!llvm::isa<llvm::InlineAsm>(CI->getCalledOperand())) {
                  for (unsigned I = 0; I < CI->arg_size(); ++I)
                    if (CI->getArgOperand(I) == V) {
                      FrameEscapes = true;
                      return;
                    }
                }
              }
              if (!llvm::isa<llvm::StoreInst>(User) &&
                  !llvm::isa<llvm::LoadInst>(User))
                CheckEscape(User);
            }
          };
      CheckEscape(AI);

      if (!FrameEscapes && !HasUnfwdLoad) {
        State.DeadFrameAllocas.insert(AI);
        for (auto *SI : FrameStores)
          State.DeadFrameStores.insert(SI);
        for (auto &[LI, Val] : Fwds) {
          State.ForwardedLoads[LI] = Val;
          State.DeadFrameStores.insert(LI);
        }
      }
    }
  }

  if (State.ForwardedLoads.empty())
    return;

  std::set<const llvm::Value *> FwdTargets;
  for (auto &[LI, Val] : State.ForwardedLoads)
    FwdTargets.insert(Val);

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
    for (auto &BB2 : Fn) {
      for (auto &Inst2 : BB2) {
        if (Inst2.getType()->isVoidTy())
          continue;
        if (llvm::isa<llvm::AllocaInst>(&Inst2))
          continue;
        if (llvm::isa<llvm::CallInst>(&Inst2))
          continue;
        if (State.DeadFrameStores.count(&Inst2))
          continue;
        if (Inst2.use_empty())
          continue;
        if (FwdTargets.count(&Inst2))
          continue;
        DeadCheckVisited.clear();
        if (AllUsersDead(&Inst2)) {
          State.DeadFrameStores.insert(&Inst2);
          Changed = true;
        }
      }
    }
  }
}

} // namespace neverd
