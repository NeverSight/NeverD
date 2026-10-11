//===- LLVMInterpreterModelMemory.cpp - Scalar LLVM model ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMInterpreterModelInternal.h"

namespace neverd::analysis::llvm_model {
void Builder::pointerProjections() {
  StateOffsets[F.getArg(0)] = 0;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &B : F)
      for (auto &I : B) {
        work();
        if (StateOffsets.count(&I))
          continue;
        const llvm::Value *Base = nullptr;
        int64_t Delta = 0;
        if (auto *G = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
          auto *Element = G->getSourceElementType();
          if (!(Element->isIntegerTy(8) || Element->isIntegerTy(16) ||
                Element->isIntegerTy(32) || Element->isIntegerTy(64)) ||
              G->getNumIndices() != 1 || G->getPointerAddressSpace() != 0 ||
              !G->getOperand(1)->getType()->isIntegerTy(64))
            fail("unsupported typed pointer projection");
          auto *Index = llvm::dyn_cast<llvm::ConstantInt>(G->getOperand(1));
          if (!Index)
            continue;
          const auto Stride =
              F.getParent()->getDataLayout().getTypeAllocSize(Element);
          // Preserve the element's allocation stride, including ABI padding.
          // A wrapped index must not disguise a poison/out-of-object GEP as
          // an ordinary byte offset into the declared state object.
          bool Overflow = false;
          const auto O = Index->getValue().smul_ov(
              llvm::APInt(64, Stride.getFixedValue()), Overflow);
          if (Overflow)
            fail("state GEP offset overflow");
          Base = G->getPointerOperand();
          Delta = O.getSExtValue();
          if (G->hasNoUnsignedWrap() && Delta < 0)
            fail("negative state GEP cannot establish unsigned no-wrap");
        } else if (I.getOpcode() == llvm::Instruction::PtrToInt ||
                   I.getOpcode() == llvm::Instruction::IntToPtr) {
          auto *IntType = I.getOpcode() == llvm::Instruction::PtrToInt
                              ? I.getType()
                              : I.getOperand(0)->getType();
          auto *PtrType = I.getOpcode() == llvm::Instruction::PtrToInt
                              ? I.getOperand(0)->getType()
                              : I.getType();
          if (!IntType->isIntegerTy(64) ||
              PtrType->getPointerAddressSpace() != 0)
            fail("lossy or nondefault pointer cast");
          Base = I.getOperand(0);
        } else if (I.getOpcode() == llvm::Instruction::Add ||
                   I.getOpcode() == llvm::Instruction::Sub) {
          auto *K = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1));
          if (!K || K->getBitWidth() != 64)
            continue;
          Base = I.getOperand(0);
          Delta = K->getSExtValue();
          if (I.getOpcode() == llvm::Instruction::Sub) {
            if (Delta == INT64_MIN)
              fail("pointer projection overflow");
            Delta = -Delta;
          }
        }
        if (!Base || !StateOffsets.count(Base))
          continue;
        if (auto *O = llvm::dyn_cast<llvm::OverflowingBinaryOperator>(&I);
            O && (O->hasNoSignedWrap() || O->hasNoUnsignedWrap()))
          fail("unproved pointer integer no-wrap obligation");
        auto Offset = StateOffsets.at(Base);
        if (Delta < -Offset || Delta >= int64_t(StateBytes) - Offset)
          fail("state pointer projection outside object");
        StateOffsets[&I] = Offset + Delta;
        Changed = true;
      }
  }
}
NdVar Builder::stateSlot(const llvm::Value *Pointer, unsigned Bytes,
                         uint64_t Align) {
  auto It = StateOffsets.find(Pointer);
  if (It == StateOffsets.end())
    fail("unknown state pointer or external memory effect");
  int64_t Off = It->second;
  if (!Bytes || Off < 0 || Off >= StateBytes || Bytes > StateBytes - Off)
    fail("memory access outside state object");
  if (Align > 8 || (Off % Align))
    fail("unproved state alignment");
  return rvar(Off, Bytes);
}
void Builder::requireGuestAlignment(LowBlock &Out, NdVar Address,
                                    uint64_t Align) {
  if (Align == 1)
    return;
  // LLVM alignment is an obligation on every reached address, not an entry
  // assumption or permission to access bytes beyond this load/store.
  auto Residue = local(8);
  emit(Out, op(NdOp::INT_AND, Residue, {Address, num(Align - 1)}));
  requireEqual(Out, Residue, num(0));
}
bool Builder::emitMemory(LowBlock &Out, const llvm::Instruction &I) {
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
    if (Load->isAtomic() || Load->isVolatile() ||
        Load->getMetadata(llvm::LLVMContext::MD_range))
      fail("unsupported load effect or contract");
    if (StateOffsets.count(Load->getPointerOperand()))
      emit(Out, op(NdOp::COPY, value(&I),
                   {stateSlot(Load->getPointerOperand(), bytes(I.getType()),
                              Load->getAlign().value())}));
    else {
      auto Address = value(Load->getPointerOperand());
      requireGuestAlignment(Out, Address, Load->getAlign().value());
      emit(Out, op(NdOp::LOAD, value(&I), {Address}));
    }
    return true;
  }
  if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
    if (Store->isAtomic() || Store->isVolatile())
      fail("unsupported store effect");
    if (StateOffsets.count(Store->getPointerOperand()))
      emit(Out, op(NdOp::COPY,
                   stateSlot(Store->getPointerOperand(),
                             bytes(Store->getValueOperand()->getType()),
                             Store->getAlign().value()),
                   {value(Store->getValueOperand())}));
    else {
      auto Address = value(Store->getPointerOperand());
      requireGuestAlignment(Out, Address, Store->getAlign().value());
      emit(Out,
           op(NdOp::STORE, {}, {Address, value(Store->getValueOperand())}));
    }
    return true;
  }
  if (I.getOpcode() == llvm::Instruction::IntToPtr ||
      I.getOpcode() == llvm::Instruction::PtrToInt) {
    auto *IT = I.getOpcode() == llvm::Instruction::PtrToInt
                   ? I.getType()
                   : I.getOperand(0)->getType();
    if (!IT->isIntegerTy(64) || bytes(I.getType()) != 8 ||
        bytes(I.getOperand(0)->getType()) != 8)
      fail("lossy guest pointer cast");
    emit(Out, op(NdOp::COPY, value(&I), {value(I.getOperand(0))}));
    return true;
  }
  return false;
}

} // namespace neverd::analysis::llvm_model
