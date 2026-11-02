//===- RegistrationFrameProofAddress.cpp - PE32 frame facts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve frame and spill addresses for the common registration proofs.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationFrameProofAddress.h"

#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameAddress.h"
#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ModRef.h"

#include <set>
#include <utility>
#include <vector>

namespace neverd::registration_frame {

AddressProof::AddressProof(
    const llvm::Function &Parent, const llvm::IntrinsicInst *Escape,
    va_t SecurityCookieVA,
    const coff_registration::RegistrationFrameStores &Stores, size_t &Work,
    bool &Exhausted, LoadProjection ProjectLoad, EdgeProjection Predecessors,
    ObjectBounds Bounds)
    : AddressProof(Parent, Escape, SecurityCookieVA, Stores, Work, Exhausted,
                   ProjectLoad, Predecessors, Bounds, {}) {}

AddressProof::AddressProof(
    const llvm::Function &Parent, const llvm::IntrinsicInst *Escape,
    va_t SecurityCookieVA,
    const coff_registration::RegistrationFrameStores &Stores, size_t &Work,
    bool &Exhausted, LoadProjection ProjectLoad, EdgeProjection Predecessors,
    ObjectBounds Bounds, MemoryProjection PreservesMemory)
    : Parent(Parent), Layout(Parent.getParent()->getDataLayout()),
      Escape(Escape), SecurityCookieVA(SecurityCookieVA), Stores(Stores),
      Work(Work), Exhausted(Exhausted), ProjectLoad(ProjectLoad),
      Predecessors(Predecessors), Bounds(Bounds),
      PreservesMemory(PreservesMemory) {}

bool AddressProof::isAliveAt(const llvm::AllocaInst &Root,
                             const llvm::Instruction &Before) {
  auto Charge = [&] {
    if (Exhausted || Work >= limits::kMaxRegistrationEHStateWork) {
      Exhausted = true;
      return false;
    }
    ++Work;
    return true;
  };
  if (!Charge() || !Root.isStaticAlloca())
    return false;
  const auto Info = Stores.lifetimeInfo(Root, Work);
  if (!Info) {
    Exhausted = true;
    return false;
  }
  if (Root.getFunction() != Before.getFunction())
    return Root.getFunction() == &Parent && !Info->HasMarkers;
  if (!Info->HasMarkers)
    return Root.getParent() != Before.getParent() || Root.comesBefore(&Before);
  const auto Key = std::make_pair(&Root, &Before);
  if (const auto Found = Alive.find(Key); Found != Alive.end())
    return Found->second;
  auto Compute = [&] {
    using Point =
        std::pair<const llvm::BasicBlock *, const llvm::Instruction *>;
    std::vector<Point> Pending{{Before.getParent(), Before.getPrevNode()}};
    std::set<Point> Seen;
    bool SawLive = false;
    while (!Pending.empty()) {
      if (!Charge())
        return false;
      const auto [Block, Last] = Pending.back();
      Pending.pop_back();
      // A backedge must revisit the suffix after the initial query prefix.
      if (!Seen.emplace(Block, Last).second)
        continue;
      const auto Marker = Stores.latestLifetime(Root, *Block, Last, Work);
      if (!Marker) {
        Exhausted = true;
        return false;
      }
      if (*Marker) {
        if ((*Marker)->getIntrinsicID() != llvm::Intrinsic::lifetime_start)
          return false;
        SawLive = true;
        continue;
      }
      if (Block == Root.getParent()) {
        // Any lifetime.start use makes the allocation initially dead,
        // including a marker in a different or unreachable block.
        if (Info->StartsDead || !Last ||
            (Last != &Root && !Root.comesBefore(Last)))
          return false;
        SawLive = true;
        continue;
      }
      if (llvm::pred_empty(Block))
        return false;
      for (const auto *Pred : llvm::predecessors(Block)) {
        if (!Charge())
          return false;
        llvm::SmallVector<const llvm::BasicBlock *, 4> Edges;
        if (Predecessors) {
          if (!Predecessors(Block, Pred, Edges) || Edges.empty())
            return false;
        } else
          Edges.push_back(Pred);
        for (const auto *Edge : Edges) {
          if (!Charge() || !Edge || Edge->getParent() != Before.getFunction())
            return false;
          Pending.emplace_back(Edge, Edge->getTerminator());
        }
      }
    }
    return SawLive;
  };
  const bool Result = Compute();
  if (!Exhausted)
    Alive.emplace(Key, Result);
  return Result;
}

bool AddressProof::preservesMemory(const llvm::Instruction &I, const Cell &Slot,
                                   uint64_t Bytes, unsigned Depth) {
  if (const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
    if (llvm::isa<llvm::LifetimeIntrinsic>(Intrinsic)) {
      if (llvm::isa<llvm::PoisonValue>(Intrinsic->getArgOperand(0)))
        return true;
      const auto Where = locate(Intrinsic->getArgOperand(0), Depth + 1);
      return Where && Where->Root != Slot.Root;
    }
    switch (Intrinsic->getIntrinsicID()) {
    case llvm::Intrinsic::stackrestore:
    case llvm::Intrinsic::write_register:
    case llvm::Intrinsic::eh_sjlj_setjmp:
      return false;
    case llvm::Intrinsic::eh_sjlj_longjmp:
      return PreservesMemory && PreservesMemory(I, Slot, Bytes);
    default:
      break;
    }
    // Query the intrinsic definition, not editable attributes on the call or
    // declaration. In particular, a forged memory(none) is no write proof.
    const auto Attributes = llvm::Intrinsic::getAttributes(
        I.getContext(), Intrinsic->getIntrinsicID(),
        Intrinsic->getFunctionType());
    if (Attributes.getMemoryEffects()
            .getWithoutLoc(llvm::IRMemLocation::InaccessibleMem)
            .onlyReadsMemory())
      return true;
  }
  return PreservesMemory && PreservesMemory(I, Slot, Bytes);
}

bool AddressProof::hasPE32Layout() const {
  return Layout.getPointerSize(0) == 4 && Layout.isLittleEndian();
}

bool AddressProof::isIndexed(const llvm::Instruction &I) const {
  return I.getModule() == Parent.getParent() && Stores.contains(I);
}

bool AddressProof::withinObject(const Cell &Where) const {
  if (Bounds)
    return Bounds(Where);
  const auto *Root = llvm::dyn_cast<llvm::AllocaInst>(Where.Root);
  const auto Bytes = Root ? Root->getAllocationSize(Layout) : std::nullopt;
  return Root && Root->isStaticAlloca() && Bytes && !Bytes->isScalable() &&
         Where.Offset >= 0 && uint64_t(Where.Offset) <= Bytes->getFixedValue();
}

std::optional<Cell> AddressProof::locate(const llvm::Value *Value,
                                         unsigned Depth) {
  if (Exhausted || Work >= limits::kMaxRegistrationEHStateWork ||
      Depth > limits::backend::kRegistrationFrameAddressLocateDepthLimit) {
    Exhausted = true;
    return std::nullopt;
  }
  ++Work;
  auto Found = Located.find(Value);
  if (Found != Located.end())
    return Found->second;
  auto Where = locateImpl(Value, Depth);
  if (!Exhausted)
    Located.emplace(Value, Where);
  return Where;
}

std::optional<Cell> AddressProof::locateImpl(const llvm::Value *Value,
                                             unsigned Depth) {
  if (Exhausted || Work >= limits::kMaxRegistrationEHStateWork ||
      Depth > ::neverd::limits::backend::
                  kRegistrationFrameAddressLocateImplDepthLimit) {
    Exhausted = true;
    return std::nullopt;
  }
  ++Work;
  if (const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Value))
    return Cell{Slot, 0};
  if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Value))
    return locate(Freeze->getOperand(0), Depth + 1);
  if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
      Global &&
      (parseNdDataSymbol(Global->getName()) ||
       parseNdCodePtrSymbol(Global->getName()) ||
       (SecurityCookieVA && Global->getName() == "__security_cookie")))
    return Cell{Global, 0};
  if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(Value);
      Call && Call->getIntrinsicID() == llvm::Intrinsic::localrecover &&
      Escape && Call->arg_size() == 3 && Call->getArgOperand(0) == &Parent) {
    const auto *Index =
        llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(2));
    if (Index && Index->getValue().ult(Escape->arg_size()))
      return locate(Escape->getArgOperand(Index->getZExtValue()), Depth + 1);
  }
  if (const auto *Cast = llvm::dyn_cast<llvm::Operator>(Value);
      Cast && llvm::Instruction::isCast(Cast->getOpcode())) {
    if (const auto *Instruction = llvm::dyn_cast<llvm::CastInst>(Value);
        Instruction && Instruction->hasPoisonGeneratingFlags())
      return std::nullopt;
    const auto *From = Cast->getOperand(0)->getType();
    const auto *To = Value->getType();
    auto FullInteger = [](const llvm::Type *Type) {
      return Type->isIntegerTy() && Type->getIntegerBitWidth() >= 32;
    };
    auto PE32Pointer = [](const llvm::Type *Type) {
      return Type->isPointerTy() && Type->getPointerAddressSpace() == 0;
    };
    switch (Cast->getOpcode()) {
    case llvm::Instruction::PtrToInt:
      if (!PE32Pointer(From) || !FullInteger(To))
        return std::nullopt;
      break;
    case llvm::Instruction::IntToPtr:
      if (!FullInteger(From) || !PE32Pointer(To))
        return std::nullopt;
      break;
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
      if (!FullInteger(From) || !FullInteger(To))
        return std::nullopt;
      break;
    case llvm::Instruction::BitCast:
      if (!PE32Pointer(From) || !PE32Pointer(To))
        return std::nullopt;
      break;
    default:
      return std::nullopt;
    }
    return locate(Cast->getOperand(0), Depth + 1);
  }
  if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Value)) {
    auto Offset = registration_frame::checkedByteGEPOffset(GEP);
    auto Base = locate(GEP->getPointerOperand(), Depth + 1);
    if (Base && llvm::isa<llvm::GlobalVariable>(Base->Root)) {
      llvm::APInt Bytes(32, 0);
      if (GEP->getPointerAddressSpace() == 0 &&
          GEP->accumulateConstantOffset(Layout, Bytes))
        Offset = Bytes.getSExtValue();
    }
    if (Base && Offset) {
      Base->Offset += *Offset;
      if (GEP->isInBounds() && !withinObject(*Base))
        return std::nullopt;
      return Base;
    }
  }
  if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
    const auto *Amount =
        llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
    if (Amount && Amount->getBitWidth() == 32 &&
        Binary->getOpcode() == llvm::Instruction::And &&
        !Binary->hasPoisonGeneratingFlags()) {
      auto Base = locate(Binary->getOperand(0), Depth + 1);
      const auto *Root =
          Base ? llvm::dyn_cast<llvm::AllocaInst>(Base->Root) : nullptr;
      auto Offset = Root ? alignX86RegistrationOffset(
                               Base->Offset, Root->getAlign().value(),
                               uint32_t(Amount->getZExtValue()))
                         : std::nullopt;
      if (Offset)
        return Cell{Root, *Offset};
    }
    if (Amount && Amount->getBitWidth() <= 64 &&
        !Binary->hasPoisonGeneratingFlags() &&
        (Binary->getOpcode() == llvm::Instruction::Add ||
         Binary->getOpcode() == llvm::Instruction::Sub)) {
      auto Base = locate(Binary->getOperand(0), Depth + 1);
      const int64_t Delta = Amount->getSExtValue();
      if (Base && Delta >= -int64_t(UINT32_MAX) &&
          Delta <= int64_t(UINT32_MAX) &&
          Base->Offset >= -int64_t(UINT32_MAX) &&
          Base->Offset <= int64_t(UINT32_MAX)) {
        // Integer address arithmetic is reduced only at the eventual
        // PE32 pointer boundary. A zero-extended -4 is 0xfffffffc, whose
        // effective byte displacement is still -4, without no-wrap flags.
        const uint32_t Next = uint32_t(Base->Offset) +
                              (Binary->getOpcode() == llvm::Instruction::Add
                                   ? uint32_t(Delta)
                                   : uint32_t(0) - uint32_t(Delta));
        Base->Offset = int64_t(llvm::APInt(32, Next).getSExtValue());
        return Base;
      }
    }
  }
  // Follow every reaching definition of an SSA register/PHI spill. All
  // normal paths must establish the same address before this exact load;
  // an entry zero or an unwritten path cannot stand in for a frame address.
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
    if (ProjectLoad) {
      auto Lookup = [&](const llvm::Value *Input) {
        return locate(Input, Depth + 1);
      };
      if (auto Root = ProjectLoad(*Load, Lookup))
        return Root;
    }
    std::optional<Cell> Common;
    auto Accept = [&](const llvm::StoreInst &Store) {
      auto Address = locate(Store.getValueOperand(), Depth + 1);
      if (!Address || (Common && *Common != *Address))
        return false;
      Common = Address;
      return true;
    };
    return visitReachingStores(*Load, Accept, Depth) ? Common : std::nullopt;
  }
  return std::nullopt;
}

bool AddressProof::visitReachingStores(
    const llvm::LoadInst &Input,
    llvm::function_ref<bool(const llvm::StoreInst &)> Visit, unsigned Depth) {
  const auto *Load = &Input;
  if (Load->isAtomic())
    return false;
  auto Slot = locate(Load->getPointerOperand(), Depth + 1);
  if (!Slot || !llvm::isa<llvm::AllocaInst>(Slot->Root))
    return false;
  const auto Read = Layout.getTypeStoreSize(Load->getType());
  return !Read.isScalable() &&
         visitReachingStoresAt(*Slot, Read.getFixedValue(), *Load, Visit,
                               Depth);
}

bool AddressProof::visitReachingStoresAt(
    const Cell &Input, uint64_t Bytes, const llvm::Instruction &Before,
    llvm::function_ref<bool(const llvm::StoreInst &)> Visit, unsigned Depth) {
  if (Exhausted || !Bytes || Bytes > UINT32_MAX ||
      Input.Offset < -int64_t(UINT32_MAX) ||
      Input.Offset > int64_t(UINT32_MAX) ||
      !llvm::isa<llvm::AllocaInst>(Input.Root) ||
      !isAliveAt(*llvm::cast<llvm::AllocaInst>(Input.Root), Before))
    return false;
  const auto *Slot = &Input;
  std::vector<std::pair<const llvm::BasicBlock *, const llvm::Instruction *>>
      Pending{{Before.getParent(), Before.getPrevNode()}};
  std::set<std::pair<const llvm::BasicBlock *, const llvm::Instruction *>> Seen;
  bool SawDefinition = false;
  while (!Pending.empty()) {
    auto [Block, Last] = Pending.back();
    Pending.pop_back();
    // The initial prefix ends immediately before the load. A backedge
    // revisits the same block's suffix, which may overwrite this cell.
    if (!Seen.emplace(Block, Last).second)
      continue;
    const auto Candidates = Stores.candidates(Slot->Root, Block, Last, Work);
    if (!Candidates) {
      Exhausted = true;
      return false;
    }
    const llvm::StoreInst *Definition = nullptr;
    for (const auto *Store : llvm::reverse(*Candidates)) {
      if (Work >= limits::kMaxRegistrationEHStateWork) {
        Exhausted = true;
        return false;
      }
      ++Work;
      auto Destination = locate(Store->getPointerOperand(), Depth + 1);
      if (!Destination || Destination->Root != Slot->Root)
        continue;
      const auto Written =
          Layout.getTypeStoreSize(Store->getValueOperand()->getType());
      if (Written.isScalable() || Written.getFixedValue() > UINT32_MAX)
        return false;
      if (Destination->Offset >= Slot->Offset + int64_t(Bytes) ||
          Slot->Offset >=
              Destination->Offset + int64_t(Written.getFixedValue()))
        continue;
      if (*Destination != *Slot || Written.getFixedValue() != Bytes ||
          Store->isAtomic() ||
          !isAliveAt(*llvm::cast<llvm::AllocaInst>(Slot->Root), *Store))
        return false;
      if (!Visit(*Store))
        return false;
      SawDefinition = true;
      Definition = Store;
      break;
    }
    const auto Effects =
        Stores.interveningEffects(Slot->Root, Block, Definition, Last, Work);
    if (!Effects) {
      Exhausted = true;
      return false;
    }
    for (const auto *Effect : *Effects) {
      if (Work >= limits::kMaxRegistrationEHStateWork) {
        Exhausted = true;
        return false;
      }
      ++Work;
      if (!preservesMemory(*Effect, *Slot, Bytes, Depth))
        return false;
    }
    if (Definition)
      continue;
    if (llvm::pred_empty(Block))
      return false;
    for (const auto *Pred : llvm::predecessors(Block)) {
      llvm::SmallVector<const llvm::BasicBlock *, 4> Edges;
      if (Predecessors) {
        if (!Predecessors(Block, Pred, Edges))
          return false;
      } else
        Edges.push_back(Pred);
      for (const auto *Edge : Edges)
        Pending.emplace_back(Edge, Edge->getTerminator());
    }
  }
  return SawDefinition;
}

} // namespace neverd::registration_frame
