//===- MBAPass.h - Mixed boolean-arithmetic obfuscation --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) mixed-boolean-arithmetic pass.  For every integer
/// add/sub/mul/and/or/xor `r = a OP b` it injects a provably-zero MBA term
/// built from the operands and adds it to the result:
///
///   r' = r + (((a ^ b) + ((a & b) << 1)) - a - b)        ; the added term ≡ 0
///
/// The added term equals zero for all `a`, `b` (the carry-save identity
/// `(a ^ b) + 2·(a & b) == a + b`), so the rewrite is exactly
/// semantics-preserving.  It is the eighth demo-level sample transform (after
/// instruction substitution, constant encryption, opaque predicates,
/// control-flow flattening, bogus control flow, indirect branches and indirect
/// calls).
///
/// Distinct from InstSubstitutionPass: that pass *replaces* an operator with an
/// equivalent expression tree; this pass *injects a zero-valued MBA polynomial*
/// into the result, the canonical mixed-boolean-arithmetic obfuscation.  The
/// injected term reuses the carry-save sub-term InstSubstitutionPass already
/// proved survives the backend's algebraic simplification, so it reaches the
/// emitted machine code rather than being folded back to zero.
///
/// It is a pure IR transform fully orthogonal to the relocation/rewrite backend
/// (L1 layer in the patch pipeline): it produces
/// ordinary machine code with ordinary fixups and runs after symbolization,
/// before codegen.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_MBAPASS_H
#define NEVERD_PASS_IR_MBAPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Injects provably-zero mixed-boolean-arithmetic terms into integer operator
/// results.
struct MBAPass : public llvm::PassInfoMixin<MBAPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of operators wrapped with an MBA term.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_MBAPASS_H
