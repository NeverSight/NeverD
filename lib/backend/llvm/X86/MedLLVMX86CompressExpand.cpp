//===- MedLLVMX86CompressExpand.cpp - Native compact memory -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The native instruction owns mask suppression, memory faults and merging.
//===----------------------------------------------------------------------===//
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86CompressExpandNative.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/X86CompactShape.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

#include <stdexcept>
#include <vector>

namespace neverd {
llvm::Value *MedLLVMEmitter::emitX86CompressExpand(const MedOp &Op,
                                                   Intrinsic Id,
                                                   llvm::IRBuilder<> &B) {
  const auto S = x86CompactMedShape(Op, TargetArch);
  if ((TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      !x86CompactShapeIsValid(Id, S))
    throw std::invalid_argument(X86CompactMalformed);
  const auto C = *x86CompactControl(Id, *S.Control);
  const unsigned Prefix = x86CompactNativePrefix(C, S.MemoryAddressSpace);
  const auto P = *x86CompactPlan(Id == Intrinsic::EVEXExpandLoad, S.Sizes[2],
                                 C.Kind, C.Zero, Prefix);
  auto *F = B.GetInsertBlock()->getParent();
  std::string Features =
      F->getFnAttribute("target-features").getValueAsString().str();
  for (const auto &Name : P.features())
    Features += (Features.empty() ? "+" : ",+") + Name;
  F->addFnAttr("target-features", Features);
  const auto Use = x86CompactOperandUse(P.Bytes / P.elementBytes(), P.Expand,
                                        P.Zero, traceSSAConst(Op.Inputs[2]));
  // Resolve only observed DS pointers or raw offsets in the selected segment.
  // Suppressed operands may be arbitrary without consuming unknown state.
  llvm::Value *Address = llvm::ConstantPointerNull::get(B.getPtrTy());
  if (Use.Address) {
    if (C.SegmentPrefix) {
      auto *Offset = B.CreateZExtOrTrunc(getRawSegmentOffset(Op.Inputs[1], B),
                                         B.getInt32Ty());
      Address = B.CreateIntToPtr(Offset, B.getPtrTy());
    } else
      Address = resolveAtomicMemoryPtr(Op.Inputs[1], 1, B.getInt8Ty(), B,
                                       S.MemoryAddressSpace);
  }
  if (Address->getType()->getPointerAddressSpace()) {
    // Transport the segment-relative offset bits. An address-space cast
    // would instead describe a linear address conversion; the native
    // instruction already owns FS/GS base addition through its prefix.
    auto *Word = B.getIntNTy(TargetArch == Arch::X64 ? 64 : 32);
    Address = B.CreateIntToPtr(B.CreatePtrToInt(Address, Word), B.getPtrTy());
  }
  auto *Vector = llvm::FixedVectorType::get(B.getInt32Ty(), P.Bytes / 4);
  auto *Data = Use.Data ? B.CreateBitCast(getVar(Op.Inputs[3], B), Vector)
                        : llvm::Constant::getNullValue(Vector);
  auto *Mask = B.CreateZExtOrTrunc(getVar(Op.Inputs[2], B), B.getInt64Ty());
  llvm::Value *Result;
  if (const auto IID = llvm::Intrinsic::lookupIntrinsicID(P.llvmName());
      IID && (!P.stackReference() || x86NativeStackMemoryIsAvailable())) {
    std::vector<llvm::Value *> Args = {Address, Mask, Data, B.getInt32(P.Kind)};
    if (P.Expand)
      Args.push_back(B.getInt32(P.Zero));
    Args.push_back(B.getInt32(P.Prefix));
    Result =
        B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, IID), Args);
  } else {
    auto *MaskType = B.getIntNTy(P.maskBits());
    auto *Return = P.Expand ? static_cast<llvm::Type *>(Vector) : B.getVoidTy();
    auto *Type = llvm::FunctionType::get(
        Return, {B.getPtrTy(), MaskType, Vector}, false);
    auto *Asm = llvm::InlineAsm::get(Type, P.assembly(), P.constraints(), true);
    Result =
        B.CreateCall(Asm, {Address, B.CreateZExtOrTrunc(Mask, MaskType), Data});
  }
  return P.Expand ? B.CreateBitCast(Result, sizeToType(P.Bytes)) : nullptr;
}
} // namespace neverd
