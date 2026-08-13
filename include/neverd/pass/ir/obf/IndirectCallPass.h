//===- IndirectCallPass.h - Indirect call obfuscation ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) indirect-call pass.  For every *direct* call to a defined,
/// non-intrinsic function `call @g(args)` it materialises `@g`'s address as a
/// PC-relative `ptrtoint(@g)`, launders it through a volatile stack slot, and
/// turns the call into an *indirect* call `call %fp(args)`.  This hides the
/// call graph from a static disassembler (the call target is no longer a
/// `bl @g` / `call rel32` with a visible symbol) while keeping exact program
/// semantics.  This is the seventh demo-level sample transform (after
/// instruction substitution, constant encryption, opaque predicates,
/// control-flow flattening, bogus control flow and indirect *branches*).
///
/// Why `ptrtoint(@g)` rather than a global function-pointer table: taking a
/// defined function's own address lowers PC-relatively (AArch64 ADRP+ADD,
/// x86-64 RIP-relative / direct, ARM PC-relative), so the construct carries no
/// absolute code pointer that would need a load-time rebase under ASLR/PIE —
/// the same reasoning as IndirectBranchPass's PIC base.  The volatile slot
/// defeats the backend's tendency to fold `inttoptr(ptrtoint(@g))` straight
/// back into a direct call (which would erase the obfuscation).
///
/// The rewrite is exactly semantics-preserving: the routed pointer always
/// equals `@g`, so the indirect call invokes the same callee with the same
/// arguments and signature (the original `CallInst`'s `FunctionType` is kept).
/// It is a pure IR transform fully orthogonal to the relocation/rewrite
/// backend (L1): it scopes
/// to defined callees so the materialised address is an in-image,
/// PC-relative reference the address model resolves without a GOT.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_OBF_INDIRECTCALLPASS_H
#define NEVERD_PASS_IR_OBF_INDIRECTCALLPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Converts direct calls to defined functions into position-independent
/// indirect calls.
struct IndirectCallPass : public llvm::PassInfoMixin<IndirectCallPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of direct calls converted to indirect calls.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_OBF_INDIRECTCALLPASS_H
