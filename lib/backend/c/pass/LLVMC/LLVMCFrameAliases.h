//===- LLVMCFrameAliases.h - Synthetic frame address analysis -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_PASS_LLVMC_FRAMEALIASES_H
#define NEVERD_LIB_BACKEND_C_PASS_LLVMC_FRAMEALIASES_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/MathExtras.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neverd::llvmc {
inline bool isSyntheticFrameAlloca(const llvm::AllocaInst *AI) {
  if (!AI)
    return false;
  const auto *Arr = llvm::dyn_cast<llvm::ArrayType>(AI->getAllocatedType());
  return Arr && Arr->getElementType()->isIntegerTy(8);
}

inline const llvm::AllocaInst *asAlloca(const llvm::Value *V) {
  if (!V)
    return nullptr;
  return llvm::dyn_cast<llvm::AllocaInst>(V->stripPointerCasts());
}

// Dead value cleanup may remove pure computations, never an observable
// instruction or a CFG edge. Unordered atomic loads are retained explicitly.
inline bool canDropDeadFrameValue(const llvm::Instruction &Inst) {
  if (Inst.isTerminator() || Inst.mayHaveSideEffects() ||
      llvm::isa<llvm::CallBase>(Inst))
    return false;
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst))
    return !Load->isVolatile() && !Load->isAtomic();
  return true;
}

inline bool frameRangesOverlap(int64_t A, uint64_t ASize, int64_t B,
                               uint64_t BSize) {
  if (!ASize || !BSize)
    return false;
  return A <= B ? uint64_t(B) - uint64_t(A) < ASize
                : uint64_t(A) - uint64_t(B) < BSize;
}

using FrameLocation = std::pair<const llvm::AllocaInst *, int64_t>;
using StoredValues =
    std::map<const llvm::AllocaInst *, std::vector<const llvm::Value *>>;

struct FrameAliases {
  std::set<FrameLocation> Locations;
  bool Incomplete = false;
  bool HasNonFrameAlternative = false;
  std::set<const llvm::AllocaInst *> Frames;
};

inline FrameAliases peelSyntheticFrames(const llvm::Value *V,
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
            Result.Frames.insert(AI);
            Result.Locations.emplace(AI, Off);
            return;
          }
          if (!FromLoad) {
            Result.HasNonFrameAlternative = true;
            return;
          }
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
        if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Cur)) {
          for (const llvm::Value *Incoming : Phi->incoming_values())
            Walk(Incoming, Off, false, Seen);
          return;
        }
        if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Cur)) {
          Walk(Select->getTrueValue(), Off, false, Seen);
          Walk(Select->getFalseValue(), Off, false, Seen);
          return;
        }
        if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Cur)) {
          if (I2P->getOperand(0)->getType()->getIntegerBitWidth() ==
              DL.getPointerTypeSizeInBits(I2P->getType()))
            return Walk(I2P->getOperand(0), Off, false, Seen);
          Result.Incomplete = true;
          return;
        }
        if (const auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(Cur)) {
          if (P2I->getType()->getIntegerBitWidth() >=
              DL.getPointerTypeSizeInBits(P2I->getOperand(0)->getType()))
            return Walk(P2I->getOperand(0), Off, false, Seen);
          Result.Incomplete = true;
          return;
        }
        if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cur)) {
          if (llvm::isa<llvm::BitCastInst, llvm::ZExtInst>(Cast))
            return Walk(Cast->getOperand(0), Off, false, Seen);
          // Truncation, sign extension and address-space conversion do not
          // establish that all address bits still name this allocation.
          Result.Incomplete = true;
          return;
        }
        if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Cur))
          return Walk(Fr->getOperand(0), Off, false, Seen);
        auto Adjust = [&](const llvm::Value *Base, int64_t Delta,
                          bool Subtract = false) {
          int64_t Adjusted;
          bool Overflow = Subtract ? llvm::SubOverflow(Off, Delta, Adjusted)
                                   : llvm::AddOverflow(Off, Delta, Adjusted);
          if (Overflow)
            Result.Incomplete = true;
          else
            Walk(Base, Adjusted, false, Seen);
        };
        if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Cur)) {
          llvm::APInt Acc(64, 0);
          if (GEP->accumulateConstantOffset(DL, Acc))
            return Adjust(GEP->getPointerOperand(), Acc.getSExtValue());
          if (GEP->getNumOperands() == 2)
            if (const auto *CI =
                    llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1)))
              return Adjust(GEP->getPointerOperand(), CI->getSExtValue());
          Result.Incomplete = true;
          return;
        }
        if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Cur)) {
          const llvm::Value *LHS = BO->getOperand(0);
          const llvm::Value *RHS = BO->getOperand(1);
          if (BO->getOpcode() == llvm::Instruction::Add) {
            if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RHS))
              return Adjust(LHS, CI->getSExtValue());
            if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(LHS))
              return Adjust(RHS, CI->getSExtValue());
          }
          if (BO->getOpcode() == llvm::Instruction::Sub)
            if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RHS))
              return Adjust(LHS, CI->getSExtValue(), true);
          Result.Incomplete = true;
          return;
        }
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur)) {
          // Only a scalar alloca carrier transports its stored addresses.
          // A value read from frame bytes is not the address of those bytes.
          if (const auto *Slot = asAlloca(LI->getPointerOperand());
              Slot && !isSyntheticFrameAlloca(Slot))
            return Walk(Slot, Off, true, Seen);
          Result.Incomplete = true;
          return;
        }
        // A literal address, including an entry zero initializer, is known
        // not to alias the synthetic frame. Keep genuinely unresolved
        // alternatives conservative below.
        if (llvm::isa<llvm::ConstantInt, llvm::ConstantPointerNull>(Cur)) {
          Result.HasNonFrameAlternative = true;
          return;
        }
        Result.Incomplete = true;
      };
  Walk(V, 0, false, {});
  if (Result.Incomplete) {
    std::set<const llvm::Value *> Seen;
    std::function<void(const llvm::Value *)> FindFrames =
        [&](const llvm::Value *Cur) {
          if (!Cur || !Seen.insert(Cur).second)
            return;
          if (const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(Cur)) {
            if (isSyntheticFrameAlloca(AI))
              Result.Frames.insert(AI);
            return;
          }
          if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur)) {
            if (auto It = Stores.find(asAlloca(LI->getPointerOperand()));
                It != Stores.end())
              for (const llvm::Value *Stored : It->second)
                FindFrames(Stored);
            return;
          }
          if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Cur))
            for (const llvm::Value *Operand : Inst->operands())
              FindFrames(Operand);
        };
    FindFrames(V);
  }
  return Result;
}

} // namespace neverd::llvmc

#endif
