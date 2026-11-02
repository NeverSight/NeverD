//===- LLVMX87ConstantAsm.h - Owned x87 constant assembly ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate control transfers and raw binary80 constant materialization.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX87CONSTANTASM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX87CONSTANTASM_H

#include "neverd/ir/X86/X87Constant.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

namespace neverd {
inline constexpr char X87ConstantAsmMetadata[] = "neverd.x87.constant-control";
inline constexpr char X87ControlConstraints[] = "r,~{memory},~{fpsr}";
inline constexpr char X87ConstantConstraints[] = "r,r,~{st},~{memory},~{fpsr}";

inline std::string x87ConstantAsm(unsigned Selector) {
  if (Selector >= std::size(X87ConstantMnemonics))
    llvm::report_fatal_error("invalid x87 constant selector");
  return std::string("fldcw ($1)\n\t") + X87ConstantMnemonics[Selector] +
         "\n\tfstpt ($0)";
}
/// A selector below seven identifies the constant; seven/eight identify the
/// control read/write. Metadata does not excuse malformed assembly or types.
inline std::optional<unsigned>
classifyX87ConstantAsm(const llvm::CallInst &Call) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Call.getMetadata(X87ConstantAsmMetadata))
    return std::nullopt;
  const auto Invalid = []() -> std::optional<unsigned> {
    llvm::report_fatal_error("invalid owned x87 constant/control assembly");
  };
  if (!Asm || !Asm->hasSideEffects() || Asm->isAlignStack() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      !Call.getType()->isVoidTy() || Call.getFunctionType()->isVarArg())
    return Invalid();
  for (const auto &Arg : Call.args())
    if (!Arg->getType()->isPointerTy() ||
        Arg->getType()->getPointerAddressSpace() != 0)
      return Invalid();
  if (Call.arg_size() == 1 &&
      Asm->getConstraintString() == X87ControlConstraints) {
    if (Asm->getAsmString() == "fnstcw ($0)")
      return 7;
    if (Asm->getAsmString() == "fldcw ($0)")
      return 8;
  }
  if (Call.arg_size() == 2 &&
      Asm->getConstraintString() == X87ConstantConstraints)
    for (unsigned I = 0; I != 7; ++I)
      if (Asm->getAsmString() == x87ConstantAsm(I))
        return I;
  return Invalid();
}
} // namespace neverd
#endif
