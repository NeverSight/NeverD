//===- MedLLVMX86SegmentMove.cpp - Native selector moves ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep ordered segment access in the native compiler interface when present.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86SegmentMoveNative.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86SegmentMove(const MedOp &Op, Intrinsic Id,
                                                llvm::IRBuilder<> &Builder) {
  const bool Read = Id == Intrinsic::ReadSegment;
  if ((TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      (!Read && Id != Intrinsic::WriteSegment) || !hasValidMedIntrinsicId(Op) ||
      Op.Inputs[0].ConstVal != unsigned(Id) || Op.NumInputs != (Read ? 2 : 3) ||
      !Op.Inputs[1].isConst() ||
      !x86SegmentMoveRegisterIsValid(Op.Inputs[1].ConstVal, Read) ||
      (!Read && (Op.Inputs[2].Size != 2 || Op.Output.Size)) ||
      (Read && Op.Output.Size && Op.Output.Size != 2) ||
      !Op.IntrinsicOutputs.empty() ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    llvm::report_fatal_error("invalid x86 segment move contract");
  const auto &Native = X86SegmentMoveInventory[Read ? 0 : 2];
  const auto Number = Op.Inputs[1].ConstVal;
  llvm::SmallVector<llvm::Value *, 2> Arguments{Builder.getInt32(Number)};
  if (!Read)
    Arguments.push_back(Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder),
                                                  Builder.getInt16Ty()));
  if (const auto ID = llvm::Intrinsic::lookupIntrinsicID(Native.LLVMName))
    return Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, ID),
                              Arguments);

  const std::string Segment = x86SegmentRegisterName(Number);
  if (Read) {
    auto *Type = llvm::FunctionType::get(Builder.getInt16Ty(), {}, false);
    return Builder.CreateCall(
        llvm::InlineAsm::get(Type, "mov %" + Segment + ", $0", "=r", true));
  }
  auto *Type = llvm::FunctionType::get(Builder.getVoidTy(),
                                       {Builder.getInt32Ty()}, false);
  return Builder.CreateCall(
      llvm::InlineAsm::get(Type, "mov ${0:w}, %" + Segment, "r,~{memory}",
                           true),
      {Builder.CreateZExt(Arguments.back(), Builder.getInt32Ty())});
}

} // namespace neverd
