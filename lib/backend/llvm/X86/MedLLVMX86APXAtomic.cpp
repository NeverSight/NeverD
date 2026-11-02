//===- MedLLVMX86APXAtomic.cpp - Native APX transactions ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve atomic memory access, conditional write-back and native faults.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86APXAtomic.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

#include <stdexcept>
#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86APXAtomic(const MedOp &Op, Intrinsic Id,
                                              llvm::IRBuilder<> &B) {
  const auto S = apxAtomicMedShape(Op, TargetArch);
  const auto Prefix = x86APXAtomicNativePrefix(S);
  if (!intrinsicApxAtomicShapeIsValid(Id, S) || !Prefix)
    throw std::invalid_argument("invalid APX atomic operand/output contract");
  const auto Plan = x86APXAtomicPlan(
      Id, S.SourceSize, TargetArch == Arch::X86 ? 32 : 64, *Prefix,
      isX86CmpccXaddIntrinsic(Id) ? S.Condition : 0, TargetArch);
  if (!Plan)
    throw std::invalid_argument("invalid APX atomic native target");
  const auto &P = *Plan;
  auto *Word = B.getIntNTy(P.addressBytes() * 8);
  llvm::Value *Offset;
  if (S.segmentPrefix())
    Offset = B.CreateZExtOrTrunc(getRawSegmentOffset(Op.Inputs[1], B), Word);
  else {
    auto *Address = resolveAtomicMemoryPtr(
        Op.Inputs[1], P.Bytes, sizeToType(P.Bytes), B, Op.MemoryAddressSpace);
    Offset = B.CreatePtrToInt(Address, Word);
  }
  std::vector<llvm::Value *> Args = {Offset};
  const auto Append = [&](unsigned I) {
    auto *V = getVar(Op.Inputs[I], B);
    auto *T = sizeToType(P.Bytes);
    Args.push_back(V->getType()->isPointerTy() ? B.CreatePtrToInt(V, T)
                                               : B.CreateZExtOrTrunc(V, T));
  };
  if (P.compare())
    Append(3); // Native compare/add order differs from normalized LowIR.
  Append(2);
  if (const auto IID = llvm::Intrinsic::lookupIntrinsicID(P.llvmName());
      IID && (!P.stackReference() || x86NativeStackMemoryIsAvailable())) {
    if (P.compare())
      Args.push_back(B.getInt32(P.Condition));
    Args.push_back(B.getInt32(P.AddressBits));
    Args.push_back(B.getInt32(P.SegmentPrefix));
    return B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, IID),
                        Args);
  }
  std::vector<llvm::Type *> Types;
  for (auto *V : Args)
    Types.push_back(V->getType());
  auto *Return = P.compare() ? sizeToType(P.Bytes) : B.getVoidTy();
  auto *Asm =
      llvm::InlineAsm::get(llvm::FunctionType::get(Return, Types, false),
                           P.assembly(), P.constraints(), true);
  return B.CreateCall(Asm, Args);
}

} // namespace neverd
