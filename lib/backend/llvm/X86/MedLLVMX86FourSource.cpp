//===- MedLLVMX86FourSource.cpp - Native four-source memory -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve all source SSA values, native arithmetic iterations and faults.
//===----------------------------------------------------------------------===//
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86FourSourceNative.h"
#include "neverd/ir/med/X86/X86FourSourceShape.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

#include <stdexcept>
#include <vector>

namespace neverd {
llvm::Value *MedLLVMEmitter::emitX86FourSource(const MedOp &Op,
                                               llvm::IRBuilder<> &B) {
  const auto S = x86FourSourceMedShape(Op, TargetArch);
  if ((TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      !x86FourSourceShapeIsValid(S))
    throw std::invalid_argument(X86FourSourceMalformed);
  const auto C = *x86FourSourceControl(*S.Control);
  const unsigned Prefix = x86FourSourceNativePrefix(C, S.MemoryAddressSpace);
  const auto P =
      *x86FourSourcePlan(static_cast<unsigned>(C.Kind), C.Zero, Prefix);
  auto *F = B.GetInsertBlock()->getParent();
  auto Features = F->getFnAttribute("target-features").getValueAsString().str();
  for (const auto &Name : P.features())
    Features += (Features.empty() ? "+" : ",+") + Name;
  F->addFnAttr("target-features", Features);
  const auto Use = x86FourSourceOperandUse(C, traceSSAConst(Op.Inputs[4]));
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
    auto *Word = B.getIntNTy(TargetArch == Arch::X64 ? 64 : 32);
    Address = B.CreateIntToPtr(B.CreatePtrToInt(Address, Word), B.getPtrTy());
  }
  auto *Vector = llvm::FixedVectorType::get(B.getInt32Ty(), C.bytes() / 4);
  auto *Zero = llvm::Constant::getNullValue(Vector);
  std::vector<llvm::Value *> Args = {
      Address, Use.Accumulator
                   ? B.CreateBitCast(getVar(Op.Inputs[2], B), Vector)
                   : Zero};
  auto *Packed = Use.Sources ? getVar(Op.Inputs[3], B) : nullptr;
  for (unsigned I = 0; I < X86FourSourceCount; ++I) {
    auto *Value =
        Packed ? B.CreateTrunc(B.CreateLShr(Packed, I * C.sourceBytes() * 8),
                               sizeToType(C.sourceBytes()))
               : nullptr;
    Args.push_back(
        Value ? B.CreateBitCast(
                    B.CreateZExtOrTrunc(Value, sizeToType(C.bytes())), Vector)
              : Zero);
  }
  Args.push_back(B.CreateZExtOrTrunc(getVar(Op.Inputs[4], B), B.getInt16Ty()));
  llvm::Value *Result;
  if (const auto ID = llvm::Intrinsic::lookupIntrinsicID(P.llvmName());
      ID && (!P.stackReference() || x86NativeStackMemoryIsAvailable())) {
    Args.push_back(B.getInt32(C.Zero));
    Args.push_back(B.getInt32(Prefix));
    Result =
        B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, ID), Args);
  } else {
    auto *Type = llvm::FunctionType::get(
        Vector,
        {B.getPtrTy(), Vector, Vector, Vector, Vector, Vector, B.getInt16Ty()},
        false);
    const auto Asm =
        llvm::InlineAsm::get(Type, P.assembly(), P.constraints(), true);
    Result = B.CreateCall(Asm, Args);
  }
  return B.CreateBitCast(Result, sizeToType(C.bytes()));
}
} // namespace neverd
