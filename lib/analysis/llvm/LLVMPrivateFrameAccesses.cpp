//===- LLVMPrivateFrameAccesses.cpp - Lifted stack addresses -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LLVMPrivateFrameAccesses.h"

#include "neverd/Common.h"
#include "neverd/backend/LLVMValueProvenance.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Local.h"

#include <optional>

namespace neverd {
using namespace frame_access;
#define LLVM_FRAME_NAME(Name, Text)                                            \
  static constexpr llvm::StringLiteral Name(Text);
#include "neverd/analysis/LLVMPrivateFrameAccesses.def"

/// Recover a constant displacement from the synthetic entry stack pointer.
/// Only transparent integer-width adapters and constant arithmetic belong to
/// this proof. Dynamic stack arithmetic remains in its original form.
static std::optional<int64_t>
privateFrameDisplacement(llvm::Value *Value, llvm::Value *EntrySP,
                         bool AllowNarrowStackView) {
  int64_t Offset = 0;
  // One path is followed at each step. Bound the walk in malformed or very
  // large lifted graphs without making a guess about an unproved address.
  for (unsigned Work = 0; Work < kMaxFrameAddressSteps; ++Work) {
    if (Value == EntrySP)
      return Offset;
    if (auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(Value)) {
      if (!AllowNarrowStackView || !Trunc->getSrcTy()->isIntegerTy(64) ||
          !Trunc->getDestTy()->isIntegerTy(32))
        return std::nullopt;
      Value = Trunc->getOperand(0);
      continue;
    }
    if (auto *ZExt = llvm::dyn_cast<llvm::ZExtInst>(Value)) {
      if (!AllowNarrowStackView || !ZExt->getSrcTy()->isIntegerTy(32) ||
          !ZExt->getDestTy()->isIntegerTy(64))
        return std::nullopt;
      Value = ZExt->getOperand(0);
      continue;
    }
    auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value);
    if (!Binary || (Binary->getOpcode() != llvm::Instruction::Add &&
                    Binary->getOpcode() != llvm::Instruction::Sub))
      return std::nullopt;
    llvm::Value *Source = Binary->getOperand(0);
    auto *Literal = llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
    if (!Literal && Binary->getOpcode() == llvm::Instruction::Add) {
      Source = Binary->getOperand(1);
      Literal = llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(0));
    }
    if (!Literal || Literal->getBitWidth() > 64)
      return std::nullopt;
    const int64_t Delta = Literal->getSExtValue();
    if (Binary->getOpcode() == llvm::Instruction::Sub && Delta == INT64_MIN)
      return std::nullopt;
    const int64_t SignedDelta =
        Binary->getOpcode() == llvm::Instruction::Sub ? -Delta : Delta;
    if ((SignedDelta > 0 && Offset > INT64_MAX - SignedDelta) ||
        (SignedDelta < 0 && Offset < INT64_MIN - SignedDelta))
      return std::nullopt;
    Offset += SignedDelta;
    Value = Source;
  }
  return std::nullopt;
}

static bool returnDependsOnProvenLoad(
    const llvm::ReturnInst &Return,
    const llvm::SmallPtrSetImpl<const llvm::LoadInst *> &ProvenLoads) {
  llvm::SmallVector<const llvm::Value *, 32> Work{Return.getReturnValue()};
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  while (!Work.empty() && Seen.size() < kMaxReturnVisited) {
    const llvm::Value *Value = Work.pop_back_val();
    if (!Value || !Seen.insert(Value).second)
      continue;
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      if (ProvenLoads.contains(Load))
        return true;
      continue;
    }
    const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value);
    if (!Inst || Inst->mayHaveSideEffects() || llvm::isa<llvm::CallBase>(Inst))
      continue;
    for (const llvm::Use &Operand : Inst->operands())
      Work.push_back(Operand.get());
    if (Work.size() > kMaxReturnWork)
      return false;
  }
  return false;
}

/// Lifting represents all native stack addresses numerically so unknown
/// aliases remain visible. An exact access wholly inside its own synthetic
/// frame has a stronger proof: it names a byte range of the frame alloca.
/// Restoring that pointer provenance lets LLVM's ordinary SROA and mem2reg
/// recover the scalar values before semantic MBA simplification. Escaping,
/// dynamic, ordered, and out-of-frame accesses keep their original pointers.
bool canonicalizePrivateFrameAccesses(llvm::Module &Mod) {
  bool Changed = false;
  const llvm::DataLayout &DL = Mod.getDataLayout();
  // Native 32-bit stack arithmetic commonly carries the synthetic pointer
  // through a 32-bit view. On a 64-bit target that truncation loses address
  // bits and cannot establish a private-frame access.
  const bool AllowNarrowStackView =
      llvm::Triple(Mod.getTargetTriple()).isArch32Bit();
  for (llvm::Function &F : Mod) {
    if (F.isDeclaration() || F.hasOptNone() ||
        !F.getEntryBlock().getName().starts_with(kFrameSetupBlock))
      continue;
    llvm::PtrToIntInst *EntrySP = nullptr;
    llvm::AllocaInst *Frame = nullptr;
    uint64_t BaseOffset = 0;
    uint64_t FrameBytes = 0;
    for (llvm::Instruction &I : F.getEntryBlock()) {
      auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(&I);
      if (!P2I || P2I->getName() != kRspInitValue)
        continue;
      auto *End =
          llvm::dyn_cast<llvm::GetElementPtrInst>(P2I->getPointerOperand());
      if (!End || End->getName() != kFrameEnd || End->getNumIndices() != 1)
        break;
      auto *Candidate =
          llvm::dyn_cast<llvm::AllocaInst>(End->getPointerOperand());
      auto *Bytes =
          Candidate
              ? llvm::dyn_cast<llvm::ArrayType>(Candidate->getAllocatedType())
              : nullptr;
      auto *Index = llvm::dyn_cast<llvm::ConstantInt>(End->getOperand(1));
      if (!Candidate || Candidate->getName() != kFrameObject ||
          !Candidate->isStaticAlloca() || !Bytes ||
          !Bytes->getElementType()->isIntegerTy(8) || !Index ||
          Index->isNegative() || Index->getBitWidth() > 64)
        break;
      BaseOffset = Index->getZExtValue();
      FrameBytes = Bytes->getNumElements();
      if (BaseOffset > FrameBytes || FrameBytes > uint64_t(INT64_MAX))
        break;
      EntrySP = P2I;
      Frame = Candidate;
      break;
    }
    if (!EntrySP || !Frame)
      continue;
    llvm::SmallVector<std::pair<llvm::Instruction *, llvm::Value *>, 16>
        Accesses;
    llvm::SmallPtrSet<const llvm::LoadInst *, 32> ProvenLoads;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      llvm::Value *Pointer = nullptr;
      llvm::Type *AccessType = nullptr;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        if (Load->isVolatile() || Load->isAtomic())
          continue;
        Pointer = Load->getPointerOperand();
        AccessType = Load->getType();
      } else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        if (Store->isVolatile() || Store->isAtomic())
          continue;
        Pointer = Store->getPointerOperand();
        AccessType = Store->getValueOperand()->getType();
      }
      auto *I2P = llvm::dyn_cast_or_null<llvm::IntToPtrInst>(Pointer);
      if (!I2P || !AccessType || !AccessType->isSized() ||
          I2P->getAddressSpace() != Frame->getAddressSpace())
        continue;
      const auto Offset = privateFrameDisplacement(I2P->getOperand(0), EntrySP,
                                                   AllowNarrowStackView);
      if (!Offset)
        continue;
      const auto Width = DL.getTypeStoreSize(AccessType);
      if (Width.isScalable() || Width.getFixedValue() > FrameBytes)
        continue;
      const int64_t First = -int64_t(BaseOffset);
      const int64_t Last =
          int64_t(FrameBytes - Width.getFixedValue()) - int64_t(BaseOffset);
      if (*Offset < First || *Offset > Last)
        continue;
      const auto Absolute = uint64_t(int64_t(BaseOffset) + *Offset);
      Accesses.emplace_back(
          &I, llvm::ConstantInt::get(llvm::Type::getInt64Ty(Mod.getContext()),
                                     Absolute));
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
        ProvenLoads.insert(Load);
    }
    if (!ProvenLoads.empty())
      for (llvm::Instruction &I : llvm::instructions(F))
        if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&I))
          if (returnDependsOnProvenLoad(*Return, ProvenLoads))
            llvm_value_provenance::markExplicitMemoryReturn(*Return);
    for (const auto &[Access, Index] : Accesses) {
      llvm::IRBuilder<> B(Access);
      llvm::Value *NewPointer = B.CreateInBoundsGEP(
          llvm::Type::getInt8Ty(Mod.getContext()), Frame, Index, kFrameSlot);
      llvm::Value *OldPointer = nullptr;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Access)) {
        OldPointer = Load->getPointerOperand();
        Load->setOperand(0, NewPointer);
      } else {
        auto *Store = llvm::cast<llvm::StoreInst>(Access);
        OldPointer = Store->getPointerOperand();
        Store->setOperand(1, NewPointer);
      }
      llvm::RecursivelyDeleteTriviallyDeadInstructions(
          llvm::cast<llvm::Instruction>(OldPointer));
      Changed = true;
    }
  }
  return Changed;
}
} // namespace neverd
