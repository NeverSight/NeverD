//===- EVMLLVMEmitterDetail.h - Private EVM LLVM backend helpers -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the module-level runtime the lowered program calls into: the
/// bounds-checked operand stack, the exponentiation loop, and the division
/// forms that give the EVM's answer for a zero divisor.
///
/// This is an implementation detail of lib/evm/emit. Nothing outside that
/// directory may include it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_EMIT_EVMLLVMEMITTERDETAIL_H
#define NEVERD_LIB_EVM_EMIT_EVMLLVMEMITTERDETAIL_H

#include "neverd/evm/emit/EVMLLVMEmitter.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"

namespace llvm {
class Function;
class Module;
class Type;
class Value;
} // namespace llvm

namespace neverd::evm::detail {

struct StackHelpers {
  llvm::Function *Push = nullptr;
  llvm::Function *Pop = nullptr;
  llvm::Function *Peek = nullptr;
  llvm::Function *Swap = nullptr;
};

llvm::ConstantInt *word(llvm::IntegerType *Type, uint64_t Value);
llvm::ConstantInt *word(llvm::IntegerType *Type, const llvm::APInt &Value);

/// Define the four internal functions that own the operand stack. Each traps
/// rather than running past its end, so a lowered program cannot read a slot
/// the EVM would have refused it.
StackHelpers buildStackHelpers(llvm::Module &Module, llvm::Type *StackType,
                               llvm::IntegerType *WordType);

/// Define the square-and-multiply loop EXP lowers to, which LLVM has no
/// intrinsic for at the EVM's word width.
llvm::Function *buildExponentHelper(llvm::Module &Module,
                                    llvm::IntegerType *WordType);

/// Division and remainder that yield zero for a zero divisor, which is the
/// EVM's answer where LLVM's would be undefined.
llvm::Value *safeUnsignedDiv(llvm::IRBuilder<> &B, llvm::Value *A,
                             llvm::Value *Divisor, bool Remainder);
llvm::Value *safeSignedDiv(llvm::IRBuilder<> &B, llvm::Value *A,
                           llvm::Value *Divisor, bool Remainder);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_EMIT_EVMLLVMEMITTERDETAIL_H
