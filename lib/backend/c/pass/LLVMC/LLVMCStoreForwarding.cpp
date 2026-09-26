//===- LLVMCStoreForwarding.cpp - Store-to-load forwarding ------*- C++ -*-===//
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

#include "LLVMCFrameAliases.h"

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <functional>
#include <optional>
#include <vector>

namespace neverd {

void analyzeStoreForwarding(LLVMCAnalysisState &State, llvm::Function &Fn) {
  State.ForwardedLoads.clear();

  using namespace llvmc;
  if (!Fn.getParent())
    return;
  const llvm::DataLayout &DL = Fn.getParent()->getDataLayout();
  StoredValues Stores;
  for (auto &BB : Fn)
    for (auto &Inst : BB)
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
        if (const auto *Slot = asAlloca(SI->getPointerOperand()))
          Stores[Slot].push_back(SI->getValueOperand());

  struct Access {
    int64_t Offset;
    uint64_t Size;
  };

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!isSyntheticFrameAlloca(AI) || State.DeadFrameAllocas.count(AI))
        continue;

      // A frame is removable only after every address use has been accounted
      // for. In particular, a stored/returned address or an invoke argument
      // escapes just as an ordinary call argument does.
      bool FrameEscapes = false;
      std::set<const llvm::Value *> Checked;
      std::function<void(const llvm::Value *)> CheckEscape =
          [&](const llvm::Value *V) {
            if (FrameEscapes || !Checked.insert(V).second)
              return;
            for (const llvm::User *User : V->users()) {
              if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(User)) {
                FrameEscapes |= SI->getValueOperand() == V;
                continue;
              }
              if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(User)) {
                FrameEscapes |= LI->getPointerOperand() != V;
                continue;
              }
              if (llvm::isa<llvm::CastInst, llvm::GetElementPtrInst,
                            llvm::BinaryOperator, llvm::PHINode,
                            llvm::SelectInst, llvm::FreezeInst>(User)) {
                CheckEscape(User);
                continue;
              }
              FrameEscapes = true;
            }
          };
      CheckEscape(AI);
      if (FrameEscapes)
        continue;

      std::map<const llvm::StoreInst *, Access> Writes;
      std::map<const llvm::LoadInst *, Access> Reads;
      bool Unresolved = false;
      auto Classify = [&](const llvm::Value *Ptr, llvm::Type *Ty,
                          bool Observable) -> std::optional<Access> {
        FrameAliases Aliases = peelSyntheticFrames(Ptr, Stores, DL);
        if (!Aliases.Frames.count(AI))
          return std::nullopt;
        const llvm::TypeSize Size = DL.getTypeStoreSize(Ty);
        if (Aliases.Incomplete || Aliases.HasNonFrameAlternative ||
            Aliases.Locations.size() != 1 ||
            Aliases.Locations.begin()->first != AI || Size.isScalable() ||
            Observable) {
          Unresolved = true;
          return std::nullopt;
        }
        return Access{Aliases.Locations.begin()->second, Size.getFixedValue()};
      };
      for (auto &AccessBB : Fn) {
        for (auto &AccessInst : AccessBB) {
          if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&AccessInst)) {
            if (auto A = Classify(SI->getPointerOperand(),
                                  SI->getValueOperand()->getType(),
                                  SI->isVolatile() || SI->isAtomic()))
              Writes.emplace(SI, *A);
          } else if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&AccessInst)) {
            if (auto A = Classify(LI->getPointerOperand(), LI->getType(),
                                  LI->isVolatile() || LI->isAtomic()))
              Reads.emplace(LI, *A);
          }
        }
      }
      if (Unresolved || Reads.empty())
        continue;

      std::map<const llvm::LoadInst *, const llvm::Value *> Forwarded;
      for (const auto &[LI, Read] : Reads) {
        unsigned Budget = 1024;
        std::set<const llvm::BasicBlock *> Active;
        std::function<const llvm::Value *(const llvm::BasicBlock *,
                                          const llvm::Instruction *)>
            ReachingValue =
                [&](const llvm::BasicBlock *Block,
                    const llvm::Instruction *Before) -> const llvm::Value * {
          if (!Budget || !Active.insert(Block).second)
            return nullptr;
          --Budget;
          const llvm::Value *Result = nullptr;
          auto End = Before ? Before->getIterator() : Block->end();
          for (auto It = End; It != Block->begin();) {
            if (!Budget) {
              Active.erase(Block);
              return nullptr;
            }
            --Budget;
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&*--It);
            auto Write = Writes.find(SI);
            if (Write == Writes.end() ||
                !frameRangesOverlap(Write->second.Offset, Write->second.Size,
                                    Read.Offset, Read.Size))
              continue;
            if (Write->second.Offset == Read.Offset &&
                Write->second.Size == Read.Size &&
                SI->getValueOperand()->getType() == LI->getType())
              Result = SI->getValueOperand();
            Active.erase(Block);
            return Result;
          }
          bool Any = false;
          for (const llvm::BasicBlock *Pred : llvm::predecessors(Block)) {
            const llvm::Value *Incoming = ReachingValue(Pred, nullptr);
            if (!Incoming || (Any && Incoming != Result)) {
              Active.erase(Block);
              return nullptr;
            }
            Result = Incoming;
            Any = true;
          }
          Active.erase(Block);
          return Any ? Result : nullptr;
        };
        const llvm::Value *Value = ReachingValue(LI->getParent(), LI);
        if (!Value) {
          Unresolved = true;
          break;
        }
        Forwarded.emplace(LI, Value);
      }
      if (Unresolved)
        continue;

      State.DeadFrameAllocas.insert(AI);
      for (const auto &[SI, _] : Writes)
        State.DeadFrameStores.insert(SI);
      for (const auto &[LI, Value] : Forwarded) {
        State.ForwardedLoads[LI] = Value;
        State.DeadFrameStores.insert(LI);
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
      if (State.Inlinable.count(UI) && canDropDeadFrameValue(*UI) &&
          AllUsersDead(UI))
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
        if (!canDropDeadFrameValue(Inst2))
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
