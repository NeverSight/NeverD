//===- X86Snp.cpp - Native SNP and SEV-ES source contracts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve explicit register completion and native RMP/GHCB memory effects.
//===----------------------------------------------------------------------===//

#include "X86Virtualization.h"

#include "neverd/ir/intrinsics/X86/X86Virtualization.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>

namespace neverd {
namespace {

std::optional<Intrinsic> nativeId(llvm::StringRef Name) {
#define NEVERD_X86_SNP(Id, Op, ...)                                            \
  if (Name == "llvm.x86." #Op ".context")                                      \
    return Intrinsic::Id;
#include "neverd/ir/intrinsics/X86/X86Snp.def"
#undef NEVERD_X86_SNP
  if (Name == "llvm.x86.vmgexit.sev")
    return Intrinsic::X86SevVmgexit;
  return {};
}

} // namespace

bool isX86SnpNativeName(llvm::StringRef Name) {
  return nativeId(Name).has_value();
}

std::string renderX86SnpExpression(Intrinsic Id,
                                   const std::vector<std::string> &Arguments,
                                   std::optional<uint64_t> Control) {
  const std::string Builtin = intrinsicCName(Id);
  if (Id == Intrinsic::X86SevVmgexit)
    return Builtin + "((uint64_t)(" + Arguments[0] + "))";
  return Builtin + "((void *)(" + Arguments[0] + "), (void *)(" + Arguments[1] +
         "), " + std::to_string(*Control) + ")";
}

std::string renderX86SnpNativeCall(Arch TargetArch, const llvm::CallBase &Call,
                                   bool ResultLive,
                                   const std::vector<std::string> &Arguments) {
  const auto *Callee = Call.getCalledFunction();
  const auto Id = Callee ? nativeId(Callee->getName()) : std::nullopt;
  if (!Id)
    throw std::invalid_argument("unknown native SNP/SEV-ES intrinsic");
  const bool Context = *Id != Intrinsic::X86SevVmgexit;
  const unsigned Count = Context ? 3 : 1;
  bool Valid =
      TargetArch == Arch::X64 && Callee->isDeclaration() &&
      !Callee->isVarArg() && Call.getCallingConv() == llvm::CallingConv::C &&
      Callee->getCallingConv() == llvm::CallingConv::C &&
      !Call.getNumOperandBundles() && Call.getType()->isVoidTy() &&
      !ResultLive && Call.arg_size() == Count && Arguments.size() == Count &&
      Call.getMemoryEffects() == llvm::MemoryEffects::unknown() &&
      !Call.doesNotReturn() && Call.hasFnAttr(llvm::Attribute::NoUnwind) &&
      Call.hasFnAttr(llvm::Attribute::Convergent) &&
      Call.hasFnAttr(llvm::Attribute::NoDuplicate);
  std::optional<uint64_t> Control;
  if (Valid && Context) {
    for (unsigned I = 0; I != 2; ++I) {
      const auto *Type = Call.getArgOperand(I)->getType();
      Valid &= Type->isPointerTy() && Type->getPointerAddressSpace() == 0;
    }
    const auto *Value =
        llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(2));
    if (Value && Value->getType()->isIntegerTy(32))
      Control = Value->getZExtValue();
    Valid &= Control && x86SnpControlIsValid(*Id, *Control);
  } else if (Valid) {
    Valid &= Call.getArgOperand(0)->getType()->isIntegerTy(64);
  }
  if (!Valid)
    throw std::invalid_argument(
        "invalid native SNP/SEV-ES completion contract");
  return renderX86SnpExpression(*Id, Arguments, Control) + ";\n";
}

} // namespace neverd
