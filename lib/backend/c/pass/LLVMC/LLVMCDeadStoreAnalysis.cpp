//===- LLVMCDeadStoreAnalysis.cpp - Dead frame-store analysis ---*- C++ -*-===//
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
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

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

bool isSyntheticFrameAlloca(const llvm::AllocaInst *AI) {
  if (!AI)
    return false;
  const auto *Arr = llvm::dyn_cast<llvm::ArrayType>(AI->getAllocatedType());
  return Arr && Arr->getElementType()->isIntegerTy(8);
}

const llvm::AllocaInst *asAlloca(const llvm::Value *V) {
  if (!V)
    return nullptr;
  return llvm::dyn_cast<llvm::AllocaInst>(V->stripPointerCasts());
}

bool isZeroSpillValue(const llvm::Value *V) {
  std::set<const llvm::Value *> Seen;
  std::function<bool(const llvm::Value *)> Rec =
      [&](const llvm::Value *Cur) -> bool {
    if (!Cur || !Seen.insert(Cur).second)
      return false;
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Cur))
      return CI->isZero();
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cur))
      return Rec(Cast->getOperand(0));
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Cur))
      return Rec(Fr->getOperand(0));
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur);
    if (!LI)
      return false;
    const llvm::AllocaInst *Slot = asAlloca(LI->getPointerOperand());
    if (!Slot || isSyntheticFrameAlloca(Slot))
      return false;
    bool Any = false;
    for (const llvm::User *U : Slot->users()) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
      if (!SI || asAlloca(SI->getPointerOperand()) != Slot)
        continue;
      const llvm::Value *Stored = SI->getValueOperand();
      if (const auto *Src = llvm::dyn_cast<llvm::LoadInst>(Stored))
        if (asAlloca(Src->getPointerOperand()) == Slot)
          continue;
      if (!Rec(Stored))
        return false;
      Any = true;
    }
    return Any;
  };
  return Rec(V);
}

using FrameLocation = std::pair<const llvm::AllocaInst *, int64_t>;
using StoredValues =
    std::map<const llvm::AllocaInst *, std::vector<const llvm::Value *>>;

struct FrameAliases {
  std::set<FrameLocation> Locations;
  bool Incomplete = false;
};

FrameAliases peelSyntheticFrames(const llvm::Value *V,
                                 const StoredValues &Stores,
                                 const llvm::DataLayout &DL) {
  FrameAliases Result;
  unsigned Visits = 0;
  std::function<void(const llvm::Value *, int64_t, bool,
                     std::set<const llvm::Value *>)>
      Walk = [&](const llvm::Value *Cur, int64_t Off, bool FromLoad,
                 std::set<const llvm::Value *> Seen) {
        if (!Cur || !Seen.insert(Cur).second || ++Visits > 128) {
          Result.Incomplete = true;
          return;
        }
        if (const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(Cur)) {
          if (isSyntheticFrameAlloca(AI)) {
            Result.Locations.emplace(AI, Off);
            return;
          }
          if (!FromLoad)
            return;
          auto It = Stores.find(AI);
          if (It == Stores.end() || It->second.empty()) {
            Result.Incomplete = true;
            return;
          }
          // A join carrier can hold different frame addresses on its normal
          // and exceptional paths. A textual last store is not its value.
          for (const llvm::Value *Stored : It->second)
            Walk(Stored, Off, false, Seen);
          return;
        }
        if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Cur))
          return Walk(I2P->getOperand(0), Off, false, Seen);
        if (const auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(Cur))
          return Walk(P2I->getOperand(0), Off, false, Seen);
        if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cur))
          return Walk(Cast->getOperand(0), Off, false, Seen);
        if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Cur))
          return Walk(Fr->getOperand(0), Off, false, Seen);
        if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Cur)) {
          llvm::APInt Acc(64, 0);
          if (GEP->accumulateConstantOffset(DL, Acc))
            return Walk(GEP->getPointerOperand(), Off + Acc.getSExtValue(),
                        false, Seen);
          if (GEP->getNumOperands() == 2)
            if (const auto *CI =
                    llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1)))
              return Walk(GEP->getPointerOperand(), Off + CI->getSExtValue(),
                          false, Seen);
          Result.Incomplete = true;
          return;
        }
        if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Cur)) {
          const llvm::Value *LHS = BO->getOperand(0);
          const llvm::Value *RHS = BO->getOperand(1);
          if (BO->getOpcode() == llvm::Instruction::Add) {
            if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RHS))
              return Walk(LHS, Off + CI->getSExtValue(), false, Seen);
            if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(LHS))
              return Walk(RHS, Off + CI->getSExtValue(), false, Seen);
          }
          if (BO->getOpcode() == llvm::Instruction::Sub)
            if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RHS))
              return Walk(LHS, Off - CI->getSExtValue(), false, Seen);
          Result.Incomplete = true;
          return;
        }
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur))
          return Walk(LI->getPointerOperand(), Off, true, Seen);
        // A literal address, including an entry zero initializer, is known
        // not to alias the synthetic frame. Keep genuinely unresolved
        // alternatives conservative below.
        if (llvm::isa<llvm::ConstantInt, llvm::ConstantPointerNull>(Cur))
          return;
        Result.Incomplete = true;
      };
  Walk(V, 0, false, {});
  return Result;
}

} // anonymous namespace

void analyzeDeadFrameStores(LLVMCAnalysisState &State, llvm::Function &Fn) {
  State.DeadFrameAllocas.clear();
  State.DeadFrameStores.clear();
  State.RawFrameLocations.clear();

  const llvm::Module *Mod = Fn.getParent();
  if (!Mod)
    return;
  const llvm::DataLayout &DL = Mod->getDataLayout();

  StoredValues Stores;
  for (auto &BB : Fn)
    for (auto &Inst : BB)
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
        if (const llvm::AllocaInst *Slot = asAlloca(SI->getPointerOperand()))
          Stores[Slot].push_back(SI->getValueOperand());

  std::set<std::pair<const llvm::AllocaInst *, int64_t>> LiveSlots;
  std::set<std::pair<const llvm::AllocaInst *, int64_t>> DirectLive;
  std::set<std::pair<const llvm::AllocaInst *, int64_t>> Plus8Live;
  std::map<FrameLocation, std::vector<const llvm::CallBase *>> Plus16Readers;
  bool UncertainFrameRead = false;
  auto NoteLive = [&](const llvm::Value *Ptr, bool ExpandRecord,
                      const llvm::CallBase *Call = nullptr) {
    FrameAliases Aliases = peelSyntheticFrames(Ptr, Stores, DL);
    if (Aliases.Locations.size() > 1 ||
        (Aliases.Incomplete && !Aliases.Locations.empty()))
      State.RawFrameLocations.insert(Aliases.Locations.begin(),
                                     Aliases.Locations.end());
    if (Aliases.Incomplete && !Aliases.Locations.empty()) {
      UncertainFrameRead = true;
    }
    for (const FrameLocation &Slot : Aliases.Locations) {
      LiveSlots.insert(Slot);
      DirectLive.insert(Slot);
      if (ExpandRecord) {
        auto Plus8 = std::make_pair(Slot.first, Slot.second + 8);
        LiveSlots.insert(Plus8);
        Plus8Live.insert(Plus8);
        auto Plus16 = std::make_pair(Slot.first, Slot.second + 16);
        LiveSlots.insert(Plus16);
        if (Call)
          Plus16Readers[Plus16].push_back(Call);
      }
    }
  };
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
        const llvm::AllocaInst *Slot = asAlloca(LI->getPointerOperand());
        if (Slot && !isSyntheticFrameAlloca(Slot))
          continue;
        NoteLive(LI->getPointerOperand(), /*ExpandRecord=*/false);
      }
      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst))
        for (const llvm::Use &U : CB->args())
          NoteLive(U.get(), /*ExpandRecord=*/true, CB);
    }
  }
  bool Grew = true;
  unsigned Guard = 0;
  while (Grew && Guard++ < 8) {
    Grew = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
        if (!SI)
          continue;
        FrameAliases Dest =
            peelSyntheticFrames(SI->getPointerOperand(), Stores, DL);
        bool DestIsLive = false;
        for (const FrameLocation &Loc : Dest.Locations)
          DestIsLive |= LiveSlots.count(Loc) != 0;
        if (!DestIsLive)
          continue;
        FrameAliases Src =
            peelSyntheticFrames(SI->getValueOperand(), Stores, DL);
        if (Src.Incomplete && !Src.Locations.empty()) {
          UncertainFrameRead = true;
          State.RawFrameLocations.insert(Src.Locations.begin(),
                                         Src.Locations.end());
        }
        for (const FrameLocation &Loc : Src.Locations) {
          bool New = LiveSlots.insert(Loc).second;
          New |= DirectLive.insert(Loc).second;
          New |= LiveSlots.insert({Loc.first, Loc.second + 8}).second;
          New |= LiveSlots.insert({Loc.first, Loc.second + 16}).second;
          if (New)
            Grew = true;
        }
      }
    }
  }

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!isSyntheticFrameAlloca(AI))
        continue;
      // A partially resolved frame read can still reach this allocation.
      // Declaring it dead would leave a printed address of an undeclared
      // synthetic frame when another predecessor is retained.
      if (UncertainFrameRead)
        continue;

      bool HasLoad = false;
      for (const auto &Slot : LiveSlots)
        if (Slot.first == AI)
          HasLoad = true;
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
              if (auto *CB = llvm::dyn_cast<llvm::CallBase>(User)) {
                for (unsigned I = 0; I < CB->arg_size(); ++I)
                  if (CB->getArgOperand(I) == V) {
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

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      bool Dead = false;
      for (auto *AI : State.DeadFrameAllocas) {
        std::set<const llvm::Value *> Visited;
        if (ptrDerivesFrom(SI->getPointerOperand(), AI, Visited)) {
          Dead = true;
          break;
        }
      }
      if (!Dead && !UncertainFrameRead) {
        FrameAliases Dest =
            peelSyntheticFrames(SI->getPointerOperand(), Stores, DL);
        if (!Dest.Incomplete && !Dest.Locations.empty()) {
          bool AllDead = true;
          bool AllZeroSpillsDead = isZeroSpillValue(SI->getValueOperand());
          for (const FrameLocation &Loc : Dest.Locations) {
            AllDead &= LiveSlots.count(Loc) == 0;
            bool Plus16ObservedAfterStore = false;
            if (auto Readers = Plus16Readers.find(Loc);
                Readers != Plus16Readers.end())
              for (const llvm::CallBase *Call : Readers->second)
                if (Call->getParent() != SI->getParent() ||
                    SI->comesBefore(Call)) {
                  Plus16ObservedAfterStore = true;
                  break;
                }
            AllZeroSpillsDead &= DirectLive.count(Loc) == 0 &&
                                 Plus8Live.count(Loc) == 0 &&
                                 !Plus16ObservedAfterStore;
          }
          Dead = AllDead || AllZeroSpillsDead;
        }
      }
      if (Dead)
        State.DeadFrameStores.insert(SI);
    }
  }

  if (State.DeadFrameStores.empty() && State.DeadFrameAllocas.empty())
    return;

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
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI);
      if (SI && SI->getValueOperand() == V) {
        const llvm::AllocaInst *Slot = asAlloca(SI->getPointerOperand());
        if (!Slot)
          return false;
        for (const llvm::User *SU : Slot->users()) {
          if (llvm::isa<llvm::StoreInst>(SU))
            continue;
          const auto *LI = llvm::dyn_cast<llvm::LoadInst>(SU);
          if (!LI)
            return false;
          if (!State.DeadFrameStores.count(LI) && !AllUsersDead(LI))
            return false;
        }
        continue;
      }
      return false;
    }
    return true;
  };

  auto AllocaLoadsAreDead = [&](const llvm::AllocaInst *Slot) {
    if (!Slot)
      return false;
    bool AnyLoad = false;
    for (const llvm::User *U : Slot->users()) {
      const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
      if (!LI)
        continue;
      AnyLoad = true;
      if (!State.DeadFrameStores.count(LI))
        return false;
    }
    return AnyLoad;
  };

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
          if (State.DeadFrameStores.count(SI))
            continue;
          const llvm::AllocaInst *Slot = asAlloca(SI->getPointerOperand());
          if (Slot && AllocaLoadsAreDead(Slot)) {
            State.DeadFrameStores.insert(SI);
            Changed = true;
          }
          continue;
        }
        if (Inst.getType()->isVoidTy())
          continue;
        if (llvm::isa<llvm::AllocaInst>(&Inst))
          continue;
        // An invoke may have no live result while its call and unwind edge
        // remain observable.
        if (llvm::isa<llvm::CallBase>(&Inst))
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
