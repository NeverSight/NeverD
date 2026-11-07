//===- LLVMX86DivRem.h - Native divide result contract -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share the native DIV/IDIV intrinsic and legacy assembly contracts with
/// LLVM-C. Both results and the possible divide error survive C projection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86DIVREM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86DIVREM_H

#include "neverd/Common.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>
#include <string>

namespace neverd {

inline constexpr char X86DivRemAsmConstraints[] =
    "={ax},={dx},{ax},{dx},r,~{flags}";

struct X86DivRemOperation {
  unsigned Bits;
  bool Signed;
};

inline const char *x86DivRemMnemonic(X86DivRemOperation Operation) {
  if (Operation.Bits == 8)
    return Operation.Signed ? "idivb" : "divb";
  if (Operation.Bits == 16)
    return Operation.Signed ? "idivw" : "divw";
  if (Operation.Bits == 32)
    return Operation.Signed ? "idivl" : "divl";
  if (Operation.Bits == 64)
    return Operation.Signed ? "idivq" : "divq";
  llvm::report_fatal_error("unsupported native x86 divide width");
}

inline std::string x86DivRemNativeName(X86DivRemOperation Operation) {
  return std::string("llvm.x86.") + (Operation.Signed ? "sdivrem" : "udivrem") +
         std::to_string(Operation.Bits);
}

inline std::optional<X86DivRemOperation>
classifyX86DivRemNative(const llvm::CallBase &Call, Arch Target) {
  const auto *Function = Call.getCalledFunction();
  if (!Function)
    return std::nullopt;
  std::optional<X86DivRemOperation> Operation;
  for (unsigned Bits : {8U, 16U, 32U, 64U})
    for (bool Signed : {false, true})
      if (Function->getName() == x86DivRemNativeName({Bits, Signed}))
        Operation = X86DivRemOperation{Bits, Signed};
  if (!Operation)
    return std::nullopt;
  const auto *Pair = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if ((Target != Arch::X86 && Target != Arch::X64) ||
      (Operation->Bits == 64 && Target != Arch::X64) || !Pair ||
      Pair->isOpaque() || Pair->getNumElements() != 2 ||
      !Pair->getElementType(0)->isIntegerTy(Operation->Bits) ||
      Pair->getElementType(1) != Pair->getElementType(0) ||
      Call.getFunctionType()->isVarArg() || Call.arg_size() != 3 ||
      Call.getFunctionType() != Function->getFunctionType() ||
      !Function->hasFnAttribute(llvm::Attribute::Convergent) ||
      !Function->hasFnAttribute(llvm::Attribute::NoDuplicate) ||
      !Function->isDeclaration() || !Call.doesNotThrow() ||
      Call.onlyReadsMemory() || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::WillReturn))
    llvm::report_fatal_error("invalid native x86 divide intrinsic contract");
  for (const auto &Argument : Call.args())
    if (!Argument->getType()->isIntegerTy(Operation->Bits))
      llvm::report_fatal_error("invalid native x86 divide intrinsic contract");
  return Operation;
}

inline std::string x86DivRemAssembly(X86DivRemOperation Operation) {
  return std::string(x86DivRemMnemonic(Operation)) + " $4";
}

inline std::optional<X86DivRemOperation>
classifyX86DivRemAsm(const llvm::CallInst &Call) {
  const auto *Assembly =
      llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Assembly)
    return std::nullopt;
  std::optional<X86DivRemOperation> Operation;
  for (unsigned Bits : {32U, 64U})
    for (bool Signed : {false, true})
      if (Assembly->getAsmString() == x86DivRemAssembly({Bits, Signed}))
        Operation = X86DivRemOperation{Bits, Signed};
  if (!Operation)
    return std::nullopt;
  const auto *Pair = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Assembly->hasSideEffects() || Assembly->isAlignStack() ||
      Assembly->getDialect() != llvm::InlineAsm::AD_ATT ||
      Assembly->getConstraintString() != X86DivRemAsmConstraints || !Pair ||
      Pair->isOpaque() || Pair->getNumElements() != 2 ||
      !Pair->getElementType(0)->isIntegerTy(Operation->Bits) ||
      Pair->getElementType(1) != Pair->getElementType(0) ||
      Call.getFunctionType()->isVarArg() || Call.arg_size() != 3)
    llvm::report_fatal_error("invalid native x86 divide assembly contract");
  for (const auto &Argument : Call.args())
    if (!Argument->getType()->isIntegerTy(Operation->Bits))
      llvm::report_fatal_error("invalid native x86 divide assembly contract");
  return Operation;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86DIVREM_H
