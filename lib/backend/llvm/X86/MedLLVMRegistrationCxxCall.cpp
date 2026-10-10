//===- MedLLVMRegistrationCxxCall.cpp - PE32 call projection --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recheck preserved callees and current direct-rethrow arguments.
//===----------------------------------------------------------------------===//
#include "MedLLVMRegistrationCxxCall.h"

#include "neverd/ir/low/RegistrationABI.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace neverd::med_llvm_eh {
std::optional<RegistrationCxxCallABI> getCheckedRegistrationCxxCallABI(
    const BinaryImage &Image, const RegistrationCalleeFrameContract &Contract,
    const RegistrationCallFrameEffect &Effect, const llvm::CallInst &Call,
    size_t &Work) {
  if (Contract.Target != Effect.Target ||
      Contract.DoesNotReturn != Effect.DoesNotReturn)
    return std::nullopt;
  auto &Context = Call.getContext();
  auto *Ptr = llvm::PointerType::get(Context, 0);
  auto *Void = llvm::Type::getVoidTy(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  RegistrationCxxCallABI ABI;
  if (Contract.isThrow()) {
    if (!Effect.DoesNotReturn || Effect.ECXFrameOffset || !Call.use_empty())
      return std::nullopt;
    if (Contract.isRuntimeRethrow()) {
      if (!getCheckedX86RegistrationThrowImportABI(Image, Effect.Target,
                                                   &Work) ||
          Call.arg_size() != 2)
        return std::nullopt;
      for (const auto &Argument : Call.args()) {
        const auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Argument.get());
        const auto *Pointer =
            llvm::dyn_cast<llvm::ConstantPointerNull>(Argument.get());
        if ((!Integer || !Integer->getType()->isIntegerTy(32) ||
             !Integer->isZero()) &&
            (!Pointer || Pointer->getType()->getPointerAddressSpace() != 0))
          return std::nullopt;
      }
      ABI.Type = llvm::FunctionType::get(Void, {Ptr, Ptr}, false);
      ABI.Convention = llvm::CallingConv::X86_StdCall;
      ABI.RuntimeRethrow = true;
    } else {
      const auto Proof =
          getCheckedX86RegistrationThrowCalleeABI(Image, Effect.Target, &Work);
      if (!Proof || Proof->IsRethrow != Contract.isRethrow())
        return std::nullopt;
      ABI.Type = llvm::FunctionType::get(Void, false);
    }
  } else {
    const auto Proof =
        getCheckedX86RegistrationLeafCalleeABI(Image, Effect.Target, &Work);
    if (!Proof || !Proof->HasIndependentScalarReturn || Proof->StackPopBytes ||
        Effect.DoesNotReturn ||
        (!Call.getType()->isIntegerTy(32) && !Call.getType()->isIntegerTy(64)))
      return std::nullopt;
    ABI.BorrowsECX = !Proof->ECXReads.empty() || !Proof->ECXWrites.empty();
    if (ABI.BorrowsECX != Effect.ECXFrameOffset.has_value())
      return std::nullopt;
    ABI.Type = ABI.BorrowsECX ? llvm::FunctionType::get(I32, {Ptr}, false)
                              : llvm::FunctionType::get(I32, false);
    ABI.Convention =
        ABI.BorrowsECX ? llvm::CallingConv::X86_ThisCall : llvm::CallingConv::C;
  }
  return ABI;
}
} // namespace neverd::med_llvm_eh
