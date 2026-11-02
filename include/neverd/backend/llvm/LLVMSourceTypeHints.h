//===- LLVMSourceTypeHints.h - Explicit source scalar spelling -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCETYPEHINTS_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCETYPEHINTS_H

#include "neverd/ir/NdTypes.h"

#include "llvm/ADT/StringRef.h"

namespace llvm {
class Argument;
} // namespace llvm

namespace neverd {
// LLVM integers are signless. These producer-supplied spelling facts preserve
// an imported C declaration without inventing signext/zeroext machine ABI
// attributes. Consumers must still validate the actual LLVM scalar width.
inline constexpr llvm::StringLiteral LLVMSourceSignedResult =
    "neverd.source.signed_result";
inline constexpr llvm::StringLiteral LLVMSourceSignedParameter =
    "neverd.source.signed_parameter";
// Exact scalar spelling for a call operand. Fixed integers select the declared
// header overload; trailing operands preserve C default argument promotions.
inline constexpr llvm::StringLiteral LLVMSourceArgumentType =
    "neverd.source.argument_type";
// A call-site contract: this argument is a read-only NUL string consumed by
// content, without retaining or observing its address. It permits literal
// spelling at that call only; it does not classify other calls to the callee.
inline constexpr llvm::StringLiteral LLVMSourceByValueStringParameter =
    "neverd.source.by_value_string";

// Opaque LLVM pointers erase pointees. This argument attribute retains only a
// source spelling: scalar pointees, named opaque records and pointer chains.
// It grants no ABI, storage extent, aliasing or dereferenceability authority.
// Argument attributes follow argument reordering; a function-wide ordinal
// table would become stale when LLVM removes a preceding argument.
inline constexpr llvm::StringLiteral LLVMSourcePointerParameter =
    "neverd.source.pointer_parameter";
void writeLLVMSourcePointerParameter(llvm::Argument &Argument,
                                     const TypeRef &Type);
TypeRef readLLVMSourcePointerParameter(const llvm::Argument &Argument);
} // namespace neverd
#endif
