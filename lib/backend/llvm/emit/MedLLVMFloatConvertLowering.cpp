//===- MedLLVMFloatConvertLowering.cpp ------------------------------------===//

#include "neverd/backend/llvm/MedLLVMFloatConvertLowering.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include <cmath>

namespace neverd::llvm_detail {

llvm::Value *emitFPToInt(llvm::IRBuilderBase &Builder, llvm::Module &Module,
                         llvm::Value *Source, llvm::Type *DestType,
                         bool IsUnsigned, Arch TargetArch) {
  llvm::Intrinsic::ID SatID =
      IsUnsigned ? llvm::Intrinsic::fptoui_sat : llvm::Intrinsic::fptosi_sat;
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, SatID, {DestType, Source->getType()});
  llvm::Value *Sat = Builder.CreateCall(Fn, {Source}, "fptoint_sat");
  if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
    return Sat;

  const unsigned Bits = DestType->getIntegerBitWidth();
  if (IsUnsigned) {
    llvm::Value *Lower = llvm::ConstantFP::get(Source->getType(), -1.0);
    llvm::Value *Upper = llvm::ConstantFP::get(
        Source->getType(), std::ldexp(1.0, static_cast<int>(Bits)));
    llvm::Value *Below =
        Builder.CreateFCmpOLE(Source, Lower, "x86_ucvt_low");
    llvm::Value *Above =
        Builder.CreateFCmpOGE(Source, Upper, "x86_ucvt_high");
    llvm::Value *Nan =
        Builder.CreateFCmpUNO(Source, Source, "x86_ucvt_nan");
    llvm::Value *Invalid = Builder.CreateOr(
        Builder.CreateOr(Below, Above, "x86_ucvt_range"), Nan,
        "x86_ucvt_invalid");
    llvm::Value *Indefinite = llvm::ConstantInt::get(
        DestType, llvm::APInt::getAllOnes(Bits));
    return Builder.CreateSelect(Invalid, Indefinite, Sat, "x86_ucvt");
  }

  llvm::Value *IntMin = llvm::ConstantInt::get(
      DestType, llvm::APInt::getSignedMinValue(Bits));
  llvm::Value *Bound = llvm::ConstantFP::get(
      Source->getType(), std::ldexp(1.0, static_cast<int>(Bits) - 1));
  llvm::Value *Overflow =
      Builder.CreateFCmpUGE(Source, Bound, "x86_cvt_ovf");
  return Builder.CreateSelect(Overflow, IntMin, Sat, "x86_cvt");
}

llvm::Value *emitIntToFP(llvm::IRBuilderBase &Builder, llvm::Value *Source,
                         llvm::Type *DestType, bool IsUnsigned,
                         Arch TargetArch) {
  if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
    return IsUnsigned ? Builder.CreateUIToFP(Source, DestType, "uitofp")
                      : Builder.CreateSIToFP(Source, DestType, "sitofp");

  const llvm::Intrinsic::ID ID =
      IsUnsigned ? llvm::Intrinsic::experimental_constrained_uitofp
                 : llvm::Intrinsic::experimental_constrained_sitofp;
  auto *Result = Builder.CreateConstrainedFPCast(
      ID, Source, DestType, {}, IsUnsigned ? "uitofp.dynamic"
                                          : "sitofp.dynamic",
      nullptr, llvm::RoundingMode::Dynamic,
      llvm::fp::ExceptionBehavior::ebStrict);
  Builder.setConstrainedFPFunctionAttr();
  Builder.setConstrainedFPCallAttr(Result);
  return Result;
}

} // namespace neverd::llvm_detail
