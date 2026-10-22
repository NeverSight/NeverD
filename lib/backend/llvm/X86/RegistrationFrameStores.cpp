//===- RegistrationFrameStores.cpp - Private PE32 spill definitions --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Skip unrelated instructions only after proving that all uses of a private
/// allocation are direct memory accesses. Aliased allocations keep the full
/// reaching-store walk in the frame proof.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationFrameStores.h"

#include "neverd/Limits.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <bit>

namespace neverd::coff_registration {
namespace {
bool charge(size_t &Work, size_t Count = 1) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Count > limits::kMaxRegistrationEHStateWork - Work)
    return false;
  Work += Count;
  return true;
}
} // namespace

bool RegistrationFrameStores::build(
    llvm::ArrayRef<const llvm::Function *> Functions, size_t &Work) {
  Stores.clear();
  DirectStores.clear();
  AllStores.clear();
  Effects.clear();
  Positions.clear();
  Lifetimes.clear();
  for (const auto *Function : Functions)
    for (const auto &Block : *Function) {
      unsigned Position = 0;
      for (const auto &I : Block) {
        if (!charge(Work))
          return false;
        Positions.try_emplace(&I, Position++);
        if (const auto *Marker = llvm::dyn_cast<llvm::LifetimeIntrinsic>(&I)) {
          const auto *Argument = Marker->getArgOperand(0);
          if (!llvm::isa<llvm::PoisonValue>(Argument)) {
            const auto *Root = llvm::dyn_cast<llvm::AllocaInst>(Argument);
            if (!Root || !Root->isStaticAlloca() ||
                Root->getFunction() != Function)
              return false;
            auto &Lifetime = Lifetimes[Root];
            Lifetime.StartsDead |=
                Marker->getIntrinsicID() == llvm::Intrinsic::lifetime_start;
            Lifetime.Markers[&Block].push_back(Marker);
          }
        }
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
          AllStores[&Block].push_back(Store);
        // LLVM's mayWriteToMemory also classifies volatile LOADs as effects
        // for optimization ordering. They do not define a different frame
        // word. Actual non-STORE writes and lifetime events remain barriers.
        else if (!llvm::isa<llvm::LoadInst>(I) &&
                 (llvm::isa<llvm::CallBase, llvm::CatchPadInst>(I) ||
                  I.mayWriteToMemory()))
          Effects[&Block].push_back(&I);
        const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!Slot || !Slot->isStaticAlloca())
          continue;
        bool Direct = true;
        for (const auto &Use : Slot->uses()) {
          if (!charge(Work))
            return false;
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Use.getUser());
              Load && Use.getOperandNo() == Load->getPointerOperandIndex())
            continue;
          if (const auto *Store =
                  llvm::dyn_cast<llvm::StoreInst>(Use.getUser());
              Store && Use.getOperandNo() == Store->getPointerOperandIndex())
            continue;
          Direct = false;
          break;
        }
        if (Direct)
          Stores.try_emplace(Slot);
      }
    }
  for (auto &[Root, Blocks] : Stores) {
    auto &Direct = DirectStores[Root];
    for (const auto *User : Root->users()) {
      if (!charge(Work))
        return false;
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
        Blocks[Store->getParent()].push_back(Store);
        Direct.push_back(Store);
      }
    }
    for (auto &[Block, Definitions] : Blocks) {
      // Sorting user-list order costs at most N log N comparisons.
      if (!charge(Work, Definitions.size() *
                            (std::bit_width(Definitions.size()) + 1)))
        return false;
      llvm::sort(Definitions, [&](const auto *Left, const auto *Right) {
        return Positions.at(Left) < Positions.at(Right);
      });
    }
  }
  return true;
}

std::optional<llvm::ArrayRef<const llvm::StoreInst *>>
RegistrationFrameStores::candidates(const llvm::Value *Root,
                                    const llvm::BasicBlock *Block,
                                    const llvm::Instruction *Through,
                                    size_t &Work) const {
  if (!charge(Work))
    return std::nullopt;
  const auto Found = Stores.find(Root);
  const auto &Blocks = Found == Stores.end() ? AllStores : Found->second;
  const auto Definitions = Blocks.find(Block);
  if (!Through || Definitions == Blocks.end())
    return llvm::ArrayRef<const llvm::StoreInst *>{};
  const auto &Candidates = Definitions->second;
  if (!charge(Work, std::bit_width(Candidates.size()) + 1))
    return std::nullopt;
  const auto End = llvm::upper_bound(Candidates, Positions.at(Through),
                                     [&](unsigned Position, const auto *I) {
                                       return Position < Positions.at(I);
                                     });
  return llvm::ArrayRef(Candidates).take_front(End - Candidates.begin());
}

std::optional<llvm::ArrayRef<const llvm::StoreInst *>>
RegistrationFrameStores::directDefinitions(const llvm::Value *Root,
                                           size_t &Work) const {
  if (!charge(Work))
    return std::nullopt;
  const auto Found = DirectStores.find(Root);
  if (Found == DirectStores.end())
    return std::nullopt;
  return llvm::ArrayRef(Found->second);
}

std::optional<llvm::ArrayRef<const llvm::Instruction *>>
RegistrationFrameStores::interveningEffects(const llvm::Value *Root,
                                            const llvm::BasicBlock *Block,
                                            const llvm::Instruction *After,
                                            const llvm::Instruction *Through,
                                            size_t &Work) const {
  if (!charge(Work))
    return std::nullopt;
  const auto Found = Effects.find(Block);
  if (!Through || Stores.count(Root) || Found == Effects.end())
    return llvm::ArrayRef<const llvm::Instruction *>{};
  const auto &Candidates = Found->second;
  if (!charge(Work, 2 * (std::bit_width(Candidates.size()) + 1)))
    return std::nullopt;
  auto BeforePosition = [&](unsigned Position, const auto *I) {
    return Position < Positions.at(I);
  };
  const auto Begin =
      After ? llvm::upper_bound(Candidates, Positions.at(After), BeforePosition)
            : Candidates.begin();
  const auto End =
      llvm::upper_bound(Candidates, Positions.at(Through), BeforePosition);
  if (Begin > End)
    return std::nullopt;
  return llvm::ArrayRef(Candidates)
      .slice(Begin - Candidates.begin(), End - Begin);
}

std::optional<RegistrationFrameStores::LifetimeInfo>
RegistrationFrameStores::lifetimeInfo(const llvm::AllocaInst &Root,
                                      size_t &Work) const {
  if (!charge(Work) || !Positions.count(&Root))
    return std::nullopt;
  const auto Found = Lifetimes.find(&Root);
  return Found == Lifetimes.end()
             ? LifetimeInfo{}
             : LifetimeInfo{true, Found->second.StartsDead};
}

std::optional<const llvm::IntrinsicInst *>
RegistrationFrameStores::latestLifetime(const llvm::AllocaInst &Root,
                                        const llvm::BasicBlock &Block,
                                        const llvm::Instruction *Through,
                                        size_t &Work) const {
  if (!charge(Work))
    return std::nullopt;
  const auto Found = Lifetimes.find(&Root);
  if (!Through || Found == Lifetimes.end())
    return nullptr;
  const auto Markers = Found->second.Markers.find(&Block);
  if (Markers == Found->second.Markers.end())
    return nullptr;
  const auto &Events = Markers->second;
  if (!charge(Work, std::bit_width(Events.size()) + 1))
    return std::nullopt;
  const auto End = llvm::upper_bound(Events, Positions.at(Through),
                                     [&](unsigned Position, const auto *I) {
                                       return Position < Positions.at(I);
                                     });
  return End == Events.begin() ? nullptr : *(End - 1);
}
} // namespace neverd::coff_registration
