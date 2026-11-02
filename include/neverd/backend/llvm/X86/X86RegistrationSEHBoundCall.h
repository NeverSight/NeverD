//===- X86RegistrationSEHBoundCall.h - Preserved SEH calls -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Bind checked preserved x86 SEH helpers to their MedIR invocations.

#ifndef NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHBOUNDCALL_H
#define NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHBOUNDCALL_H

#include "neverd/ir/med/X86/X86RegistrationSEHBoundCall.h"

#include "llvm/IR/CallingConv.h"

#include <map>
#include <optional>

namespace llvm {
class FunctionType;
class LLVMContext;
} // namespace llvm

namespace neverd {
struct MedFunc;
struct MedOp;

llvm::FunctionType *
getX86RegistrationSEHBoundType(const RegistrationSEHBoundCall &Call,
                               llvm::LLVMContext &Context);

llvm::CallingConv::ID
getX86RegistrationSEHBoundConvention(const RegistrationSEHBoundCall &Call);

/// Empty for a preserved source helper; otherwise the authenticated import.
llvm::StringRef
getX86RegistrationSEHBoundImport(const RegistrationSEHBoundCall &Call);

/// A validated IAT invocation uses dllimport transport. A direct runtime
/// veneer retains its original code target and ordinary CALL relocation.
bool usesX86RegistrationSEHImportSlot(const X86RegistrationSEHBoundCall &Call);

} // namespace neverd

#endif
