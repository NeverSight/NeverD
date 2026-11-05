//===- X64StateObjectPointers.cpp - Explicit state object access ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover bounded object pointers without reinterpreting guest arithmetic.
//===----------------------------------------------------------------------===//

#include "X64StateObjectPointers.h"

#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Local.h"

#include <optional>
#include <utility>

namespace neverd {
namespace {

std::optional<uint64_t> displacement(const llvm::BinaryOperator &Op,
                                     const llvm::Value *Base, uint64_t Offset,
                                     uint64_t Bytes) {
  if (!Op.getType()->isIntegerTy(64) ||
      (Op.getOpcode() != llvm::Instruction::Add &&
       Op.getOpcode() != llvm::Instruction::Sub) ||
      Op.hasNoUnsignedWrap() || Op.hasNoSignedWrap())
    return std::nullopt;
  const bool First = Op.getOperand(0) == Base;
  const auto *Constant =
      llvm::dyn_cast<llvm::ConstantInt>(Op.getOperand(First ? 1 : 0));
  if (!Constant)
    return std::nullopt;
  const uint64_t Delta = Constant->getZExtValue();
  if (Op.getOpcode() == llvm::Instruction::Sub)
    return First && Delta <= Offset ? std::optional(Offset - Delta)
                                    : std::nullopt;
  return Delta <= Bytes - Offset ? std::optional(Offset + Delta) : std::nullopt;
}

/// This worklist follows only the state pointer's exact-width def/use graph.
/// A loaded guest address, a variable index or a PHI supplies no object proof.
void projectFunction(llvm::Function &Function, uint64_t StateBytes) {
  if (Function.isDeclaration() || Function.arg_empty())
    return;
  auto *State = Function.getArg(0);
  const auto *PointerType = llvm::dyn_cast<llvm::PointerType>(State->getType());
  if (!PointerType || PointerType->getAddressSpace() != 0)
    return;
  const auto &Layout = Function.getParent()->getDataLayout();
  llvm::SmallVector<std::pair<llvm::Value *, uint64_t>, 32> Pending{{State, 0}};
  llvm::SmallPtrSet<llvm::Value *, 32> Seen;
  llvm::SmallVector<std::pair<llvm::Instruction *, uint64_t>, 32> Accesses;
  Seen.insert(State);
  while (!Pending.empty()) {
    const auto [Value, Offset] = Pending.pop_back_val();
    for (auto *User : Value->users()) {
      auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (!Instruction)
        continue;
      std::optional<uint64_t> Next;
      if (const auto *Cast = llvm::dyn_cast<llvm::PtrToIntInst>(Instruction)) {
        if (Cast->getType()->isIntegerTy(64) &&
            Cast->getPointerAddressSpace() == 0)
          Next = Offset;
      } else if (const auto *Cast =
                     llvm::dyn_cast<llvm::IntToPtrInst>(Instruction)) {
        if (Cast->getOperand(0)->getType()->isIntegerTy(64) &&
            Cast->getAddressSpace() == 0)
          Next = Offset;
      } else if (const auto *Op =
                     llvm::dyn_cast<llvm::BinaryOperator>(Instruction)) {
        Next = displacement(*Op, Value, Offset, StateBytes);
      }
      if (Next && Seen.insert(Instruction).second)
        Pending.emplace_back(Instruction, *Next);
      llvm::Type *AccessType = nullptr;
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Instruction)) {
        if (Load->getPointerOperand() == Value && !Load->isVolatile() &&
            !Load->isAtomic())
          AccessType = Load->getType();
      } else if (const auto *Store =
                     llvm::dyn_cast<llvm::StoreInst>(Instruction)) {
        if (Store->getPointerOperand() == Value && !Store->isVolatile() &&
            !Store->isAtomic())
          AccessType = Store->getValueOperand()->getType();
      }
      if (!AccessType || !AccessType->isSized())
        continue;
      const auto Width = Layout.getTypeStoreSize(AccessType);
      if (!Width.isScalable() && Width.getFixedValue() <= StateBytes - Offset)
        Accesses.emplace_back(Instruction, Offset);
    }
  }
  for (const auto [Access, Offset] : Accesses) {
    llvm::IRBuilder<> Builder(Access);
    // The complete access is inside the producer-declared live object. This
    // is the authority for inbounds, not a numeric guest-address inference.
    llvm::Value *Pointer = State;
    if (Offset)
      Pointer = Builder.CreateConstInBoundsGEP1_64(Builder.getInt8Ty(), State,
                                                   Offset, "state_field");
    const unsigned Index = llvm::isa<llvm::LoadInst>(Access) ? 0 : 1;
    auto *Old = Access->getOperand(Index);
    Access->setOperand(Index, Pointer);
    if (auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Old))
      llvm::RecursivelyDeleteTriviallyDeadInstructions(Instruction);
  }
}

} // namespace

void preserveX64StateObjectPointers(llvm::Module &Module, uint64_t StateBytes) {
  if (!StateBytes || StateBytes > uint64_t(INT64_MAX) ||
      Module.getDataLayout().getPointerSizeInBits(0) != 64 ||
      Module.getDataLayout().getIndexSizeInBits(0) != 64)
    return;
  // Reuse the shared SSA owner; no new address-alias or reaching-store solver.
  // Guest loads, stores, ordering and effects are not optimized away here.
  Pipeline::promoteScaffoldingAllocas(Module);
  for (auto &Function : Module)
    projectFunction(Function, StateBytes);
}

} // namespace neverd
