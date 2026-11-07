//===- LLVMX86BitTest.h - Native memory bit contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate native signed-index memory bit strings before C projection.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86BITTEST_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86BITTEST_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ModRef.h"

#include <optional>
#include <string>

namespace neverd {

struct X86BitTestOperation {
  unsigned Bits;
  const char *Mnemonic;
};

inline std::string x86BitTestNativeName(X86BitTestOperation Operation) {
  return std::string("llvm.x86.") + Operation.Mnemonic + ".mem" +
         std::to_string(Operation.Bits);
}

inline std::optional<X86BitTestOperation>
classifyX86BitTestNative(const llvm::CallBase &Call, Arch Target) {
  const auto *Function = Call.getCalledFunction();
  if (!Function)
    return std::nullopt;
  llvm::StringRef Name = Function->getName();
  if (!Name.consume_front("llvm.x86.bt"))
    return std::nullopt;
  const char *Mnemonic;
  if (Name.consume_front(".mem"))
    Mnemonic = "bt";
  else if (Name.consume_front("s.mem"))
    Mnemonic = "bts";
  else if (Name.consume_front("r.mem"))
    Mnemonic = "btr";
  else if (Name.consume_front("c.mem"))
    Mnemonic = "btc";
  else
    return std::nullopt;
  const unsigned Bits = Name == "16"   ? 16
                        : Name == "32" ? 32
                        : Name == "64" ? 64
                                       : 0;
  if (!Bits)
    return std::nullopt;
  const X86BitTestOperation Operation{Bits, Mnemonic};
  if ((Target != Arch::X86 && Target != Arch::X64) ||
      (Operation.Bits == 64 && Target != Arch::X64) ||
      !Call.getType()->isIntegerTy(8) || Call.arg_size() != 2 ||
      Call.getFunctionType()->isVarArg() ||
      Call.getFunctionType() != Function->getFunctionType() ||
      !Function->isDeclaration() ||
      !Function->hasFnAttribute(llvm::Attribute::Convergent) ||
      !Function->hasFnAttribute(llvm::Attribute::NoDuplicate) ||
      !Call.doesNotThrow() || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::WillReturn) ||
      Call.hasFnAttr(llvm::Attribute::Speculatable) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getNumOperandBundles() != 0)
    llvm::report_fatal_error(
        "invalid native x86 bit-string intrinsic contract");
  const auto *Pointer =
      llvm::dyn_cast<llvm::PointerType>(Call.getArgOperand(0)->getType());
  if (!Pointer || Pointer->getAddressSpace() != 0 ||
      !Call.getArgOperand(1)->getType()->isIntegerTy(Operation.Bits))
    llvm::report_fatal_error(
        "invalid native x86 bit-string intrinsic contract");
  return Operation;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86BITTEST_H
