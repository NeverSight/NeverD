//===- LLVMX86ShadowStackAsm.h - Owned RDSSP assembly ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMX86SHADOWSTACKASM_H
#define NEVERD_BACKEND_LLVM_LLVMX86SHADOWSTACKASM_H

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>
#include <string>

namespace neverd {
inline constexpr char X86ShadowStackReadMetadata[] = "neverd.x86.rdssp";
inline constexpr char X86ShadowStackReadConstraints[] = "=r,0,~{memory}";

inline std::string x86ShadowStackReadAsm(unsigned Word, unsigned Width) {
  return Width == 8 ? "rdsspq $0" : Word == 8 ? "rdsspd ${0:k}" : "rdsspd $0";
}

/// Metadata never upgrades an untyped mnemonic: result and tied old input
/// must have the same full-register type, with no other operand or effects.
inline std::optional<unsigned>
classifyX86ShadowStackReadAsm(const llvm::CallInst &Call) {
  const auto *Mark = Call.getMetadata(X86ShadowStackReadMetadata);
  if (!Mark)
    return std::nullopt;
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (Mark->getNumOperands() == 0 && Asm && Asm->hasSideEffects() &&
      !Asm->isAlignStack() && !Asm->canThrow() &&
      Asm->getDialect() == llvm::InlineAsm::AD_ATT &&
      Asm->getConstraintString() == X86ShadowStackReadConstraints &&
      Call.arg_size() == 1 &&
      Call.getArgOperand(0)->getType() == Call.getType())
    for (unsigned Word : {4U, 8U})
      for (unsigned Width : {4U, 8U})
        if ((Width == 4 || Word == 8) &&
            Call.getType()->isIntegerTy(Word * 8) &&
            Asm->getAsmString() == x86ShadowStackReadAsm(Word, Width))
          return Width;
  llvm::report_fatal_error("invalid x86 shadow stack read assembly contract");
}
} // namespace neverd

#endif
