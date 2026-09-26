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

#include "LLVMCFrameAliases.h"

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace {

using namespace llvmc;

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

} // anonymous namespace

void analyzeDeadFrameStores(LLVMCAnalysisState &State, llvm::Function &Fn) {
  using namespace llvmc;
  State.DeadFrameAllocas.clear();
  State.DeadFrameStores.clear();
  State.RawFrameLocations.clear();
  State.RawFrameAllocas.clear();

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
  std::map<FrameLocation, uint64_t> DirectReadSizes;
  auto OverlapsRead = [&](FrameLocation Store, uint64_t Size) {
    for (const auto &[Read, ReadSize] : DirectReadSizes)
      if (Store.first == Read.first &&
          frameRangesOverlap(Store.second, Size, Read.second, ReadSize))
        return true;
    return false;
  };
  bool UncertainFrameRead = false;
  auto NoteLive = [&](const llvm::Value *Ptr, bool ExpandRecord,
                      const llvm::CallBase *Call = nullptr,
                      uint64_t ReadSize = 0) {
    FrameAliases Aliases = peelSyntheticFrames(Ptr, Stores, DL);
    if (Aliases.Locations.size() > 1 ||
        (Aliases.HasNonFrameAlternative && !Aliases.Locations.empty()) ||
        (Aliases.Incomplete && !Aliases.Frames.empty()))
      State.RawFrameLocations.insert(Aliases.Locations.begin(),
                                     Aliases.Locations.end());
    if (Aliases.Incomplete && !Aliases.Frames.empty()) {
      UncertainFrameRead = true;
    }
    for (const FrameLocation &Slot : Aliases.Locations) {
      LiveSlots.insert(Slot);
      DirectLive.insert(Slot);
      if (ReadSize)
        DirectReadSizes[Slot] = std::max(DirectReadSizes[Slot], ReadSize);
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
        const llvm::TypeSize Size = DL.getTypeStoreSize(LI->getType());
        if (Size.isScalable())
          UncertainFrameRead = true;
        NoteLive(LI->getPointerOperand(), /*ExpandRecord=*/false, nullptr,
                 Size.isScalable() ? 0 : Size.getFixedValue());
      }
      if (const auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&Inst))
        NoteLive(RMW->getPointerOperand(), /*ExpandRecord=*/false, nullptr,
                 DL.getTypeStoreSize(RMW->getValOperand()->getType())
                     .getFixedValue());
      if (const auto *CX = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&Inst))
        NoteLive(CX->getPointerOperand(), /*ExpandRecord=*/false, nullptr,
                 DL.getTypeStoreSize(CX->getCompareOperand()->getType())
                     .getFixedValue());
      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        for (const llvm::Use &U : CB->args()) {
          NoteLive(U.get(), /*ExpandRecord=*/true, CB);
          if (CB->isInlineAsm() && CB->mayReadOrWriteMemory()) {
            // A memory asm operand is the start of a possibly dynamic span,
            // not a scalar home. Splitting its allocation would detach indexed
            // accesses and can turn REP STOS into an out-of-bounds scalar
            // write.
            const FrameAliases Aliases =
                peelSyntheticFrames(U.get(), Stores, DL);
            State.RawFrameAllocas.insert(Aliases.Frames.begin(),
                                         Aliases.Frames.end());
          }
        }
      }
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
        const llvm::TypeSize Size =
            DL.getTypeStoreSize(SI->getValueOperand()->getType());
        for (const FrameLocation &Loc : Dest.Locations)
          DestIsLive |= LiveSlots.count(Loc) != 0 || Size.isScalable() ||
                        OverlapsRead(Loc, Size.getFixedValue());
        if (!DestIsLive)
          continue;
        FrameAliases Src =
            peelSyntheticFrames(SI->getValueOperand(), Stores, DL);
        if (Src.Incomplete && !Src.Frames.empty()) {
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

  std::set<const llvm::AllocaInst *> EscapedFrames = State.RawFrameAllocas;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!isSyntheticFrameAlloca(AI))
        continue;
      if (UncertainFrameRead || State.RawFrameAllocas.count(AI))
        continue;

      bool HasLoad = false;
      bool Escapes = false;
      for (const auto &Slot : LiveSlots)
        if (Slot.first == AI)
          HasLoad = true;
      std::set<const llvm::Value *> Visited;
      std::function<void(const llvm::Value *)> Check =
          [&](const llvm::Value *V) {
            if (!Visited.insert(V).second)
              return;
            for (const llvm::User *User : V->users()) {
              if (llvm::isa<llvm::LoadInst>(User)) {
                HasLoad = true;
                continue;
              }
              if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(User)) {
                if (SI->isVolatile() || SI->isAtomic()) {
                  HasLoad = true;
                  Escapes |= SI->getValueOperand() == V;
                }
                if (SI->getValueOperand() == V) {
                  // Ordinary scalar allocas carry frame addresses internally.
                  // Follow their reloads, but an externally stored address lets
                  // the consumer observe every byte in the original frame.
                  const auto *Slot = asAlloca(SI->getPointerOperand());
                  if (!Slot || isSyntheticFrameAlloca(Slot)) {
                    Escapes = true;
                  } else {
                    for (const llvm::User *CarrierUser : Slot->users()) {
                      if (const auto *LI =
                              llvm::dyn_cast<llvm::LoadInst>(CarrierUser)) {
                        Check(LI);
                      } else if (const auto *Store =
                                     llvm::dyn_cast<llvm::StoreInst>(
                                         CarrierUser)) {
                        Escapes |= Store->getPointerOperand() != Slot;
                      } else {
                        Escapes = true;
                      }
                    }
                  }
                }
                if (SI->getPointerOperand() == V) {
                  const FrameAliases Dest = peelSyntheticFrames(V, Stores, DL);
                  HasLoad |= Dest.Incomplete || Dest.HasNonFrameAlternative;
                }
                continue;
              }
              if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(User)) {
                for (const llvm::Use &Arg : CB->args())
                  HasLoad |= Arg.get() == V;
                continue;
              }
              if (llvm::isa<llvm::ReturnInst>(User)) {
                Escapes = true;
                continue;
              }
              if (llvm::isa<llvm::CastInst, llvm::GetElementPtrInst,
                            llvm::BinaryOperator, llvm::PHINode,
                            llvm::SelectInst, llvm::FreezeInst>(User)) {
                Check(User);
              } else {
                // Pointer identity and observable memory instructions also need
                // the allocation to remain, even without an ordinary load.
                HasLoad = true;
              }
            }
          };
      Check(AI);
      if (Escapes)
        EscapedFrames.insert(AI);
      if (!HasLoad && !Escapes)
        State.DeadFrameAllocas.insert(AI);
    }
  }

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI || SI->isVolatile() || SI->isAtomic())
        continue;
      const FrameAliases Dest =
          peelSyntheticFrames(SI->getPointerOperand(), Stores, DL);
      const bool AllFrame = !Dest.Incomplete && !Dest.HasNonFrameAlternative &&
                            !Dest.Locations.empty();
      bool Dead = AllFrame;
      bool Escapes = false;
      for (const FrameLocation &Loc : Dest.Locations) {
        Dead &= State.DeadFrameAllocas.count(Loc.first) != 0;
        Escapes |= EscapedFrames.count(Loc.first) != 0;
      }
      if (!Dead && !UncertainFrameRead && !Escapes) {
        if (AllFrame) {
          const llvm::TypeSize Size =
              DL.getTypeStoreSize(SI->getValueOperand()->getType());
          bool AllDead = !Size.isScalable();
          bool AllZeroSpillsDead = isZeroSpillValue(SI->getValueOperand());
          for (const FrameLocation &Loc : Dest.Locations) {
            const bool ReadOverlap =
                Size.isScalable() || OverlapsRead(Loc, Size.getFixedValue());
            AllDead &= LiveSlots.count(Loc) == 0 && !ReadOverlap;
            bool Plus16ObservedAfterStore = false;
            if (auto Readers = Plus16Readers.find(Loc);
                Readers != Plus16Readers.end())
              for (const llvm::CallBase *Call : Readers->second)
                if (Call->getParent() != SI->getParent() ||
                    SI->comesBefore(Call)) {
                  Plus16ObservedAfterStore = true;
                  break;
                }
            AllZeroSpillsDead &= !ReadOverlap && DirectLive.count(Loc) == 0 &&
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
      if (State.Inlinable.count(UI) && canDropDeadFrameValue(*UI) &&
          AllUsersDead(UI))
        continue;
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI);
      if (SI && !SI->isVolatile() && !SI->isAtomic() &&
          SI->getValueOperand() == V) {
        const llvm::AllocaInst *Slot = asAlloca(SI->getPointerOperand());
        if (!Slot)
          return false;
        for (const llvm::User *SU : Slot->users()) {
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(SU)) {
            if (Store->getPointerOperand() == Slot &&
                Store->getValueOperand() != Slot)
              continue;
            return false;
          }
          const auto *LI = llvm::dyn_cast<llvm::LoadInst>(SU);
          if (!LI)
            return false;
          if (!canDropDeadFrameValue(*LI) ||
              (!State.DeadFrameStores.count(LI) && !AllUsersDead(LI)))
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
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(U)) {
        if (Store->getPointerOperand() == Slot &&
            Store->getValueOperand() != Slot)
          continue;
        return false;
      }
      const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
      if (!LI)
        return false;
      AnyLoad = true;
      if (!canDropDeadFrameValue(*LI) || !State.DeadFrameStores.count(LI))
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
          if (SI->isVolatile() || SI->isAtomic() ||
              State.DeadFrameStores.count(SI))
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
        if (!canDropDeadFrameValue(Inst))
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
