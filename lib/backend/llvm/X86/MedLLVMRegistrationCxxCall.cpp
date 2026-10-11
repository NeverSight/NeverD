//===- MedLLVMRegistrationCxxCall.cpp - PE32 call projection --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recheck preserved callees and current CRT throw arguments.
//===----------------------------------------------------------------------===//
#include "MedLLVMRegistrationCxxCall.h"

#include "neverd/ir/low/RegistrationABI.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace neverd::med_llvm_eh {
namespace {
std::optional<va_t> throwInfoAddress(const llvm::Value *Value,
                                     const llvm::DataLayout &Layout) {
  if (const auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Value))
    return Integer->getBitWidth() == 32
               ? std::optional<va_t>(Integer->getZExtValue())
               : std::nullopt;
  if (const auto *Cast = llvm::dyn_cast<llvm::ConstantExpr>(Value);
      Cast && Cast->getOpcode() == llvm::Instruction::PtrToInt &&
      Cast->getType()->isIntegerTy(32))
    Value = Cast->getOperand(0);
  if (!Value->getType()->isPointerTy() ||
      Value->getType()->getPointerAddressSpace())
    return std::nullopt;
  llvm::APInt Offset(Layout.getIndexTypeSizeInBits(Value->getType()), 0);
  const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(
      Value->stripAndAccumulateConstantOffsets(Layout, Offset, true));
  if (!Global || !Global->isDeclaration() || !Global->hasExternalLinkage() ||
      Global->isThreadLocal() || Global->getAddressSpace() ||
      Global->hasDLLImportStorageClass())
    return std::nullopt;
  auto Base = parseNdDataSymbol(Global->getName());
  if (!Base)
    Base = parseNdCodePtrSymbol(Global->getName());
  if (!Base || *Base > UINT32_MAX || !Offset.isSignedIntN(33))
    return std::nullopt;
  const auto Address = int64_t(*Base) + Offset.getSExtValue();
  return Address >= 0 && Address <= UINT32_MAX ? std::optional<va_t>(Address)
                                               : std::nullopt;
}
} // namespace

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
    if (Contract.isRuntimeThrow()) {
      if (!getCheckedX86RegistrationThrowImportABI(Image, Effect.Target,
                                                   &Work) ||
          Call.arg_size() != 2)
        return std::nullopt;
      if (!Effect.RuntimeThrow)
        return std::nullopt;
      for (const auto &Argument : Call.args())
        if (!Argument->getType()->isIntegerTy(32) &&
            (!Argument->getType()->isPointerTy() ||
             Argument->getType()->getPointerAddressSpace() != 0))
          return std::nullopt;
      if (Effect.RuntimeThrow->isRethrow()) {
        for (const auto &Argument : Call.args()) {
          const auto *Integer =
              llvm::dyn_cast<llvm::ConstantInt>(Argument.get());
          const auto *Pointer =
              llvm::dyn_cast<llvm::ConstantPointerNull>(Argument.get());
          if ((!Integer || !Integer->isZero()) && !Pointer)
            return std::nullopt;
        }
      } else {
        const auto &Throw = *Effect.RuntimeThrow;
        const auto Info = coff_loader::getCheckedX86SimpleCxxThrowInfo(
            Image, Throw.ThrowInfoVA);
        const auto Table = throwInfoAddress(Call.getArgOperand(1),
                                            Call.getModule()->getDataLayout());
        if (!Info || Info->ObjectSize != Throw.ObjectSize || !Table ||
            *Table != Throw.ThrowInfoVA ||
            llvm::isa<llvm::Constant>(Call.getArgOperand(0)))
          return std::nullopt;
        ABI.ThrowInfoVA = Throw.ThrowInfoVA;
      }
      ABI.Type = llvm::FunctionType::get(Void, {Ptr, Ptr}, false);
      ABI.Convention = llvm::CallingConv::X86_StdCall;
      ABI.RuntimeThrow = true;
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
