//===- LLVMCallContract.h - resolved call signature validation ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H
#define NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H

namespace llvm {
class Module;
}

namespace neverd {
class LLVMSourceMap;
/// Reconcile calls whose exact immutable target becomes known after emission.
/// Drop surplus fixed arguments and adapt scalar integer/pointer carriers.
/// Missing arguments, incompatible carriers and musttail changes fail clearly.
bool normalizeResolvedLLVMCalls(llvm::Module &Module,
                                LLVMSourceMap *Sources = nullptr);
/// Check identities after deferred import placeholders have been promoted.
/// Opaque pointers otherwise let a mismatched direct call pass LLVM's verifier.
/// Check both the type and calling convention after optimization and linking.
/// An inconsistent resolved target emits a diagnostic and returns false.
bool validateResolvedLLVMCallSignatures(llvm::Module &Module);
} // namespace neverd

#endif
