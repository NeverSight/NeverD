//===- X86RegistrationLocalUnwind.h - CRT local unwind bridge -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares the physical CRT bridge for a compiler-owned SEH registration.

#ifndef NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONLOCALUNWIND_H
#define NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONLOCALUNWIND_H

#include "neverd/ir/RegistrationCall.h"

#include "llvm/Support/Error.h"

#include <string>

namespace llvm {
class Function;
class FunctionType;
class LLVMContext;
} // namespace llvm

namespace neverd::x86_registration {

std::string localUnwindBridgeName(va_t Target, va_t Owner);

llvm::FunctionType *
localUnwindRuntimeType(llvm::LLVMContext &Context,
                       RegistrationLocalUnwindContract::Kind Protocol);

/// Build a private full-unwind bridge for an authenticated CRT declaration.
/// The first argument must be the compiler-owned registration captured at the
/// parent's entry, not the callback's current FS head or a synthetic frame.
/// EH4 also takes the authenticated image cookie address. Call-site binding
/// and private capture recovery require separate source and native proofs.
llvm::Expected<llvm::Function *>
createLocalUnwindBridge(llvm::Function &Runtime,
                        RegistrationLocalUnwindContract::Kind Protocol,
                        va_t Owner = 0);

} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONLOCALUNWIND_H
