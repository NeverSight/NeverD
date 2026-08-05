//===- InstSubstitutionPass.h - Instruction substitution pass ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) instruction-substitution pass.  Replaces integer
/// binary operators (add / sub / and / or / xor) with semantically-equivalent
/// instruction sequences.  This is a deliberately small, demo-level sample
/// transform that shows how an IR pass plugs into the patch pipeline — more
/// substantial transforms will be added as separate passes later.
///
/// It is a pure IR transform: it produces ordinary machine code + ordinary
/// fixups, so it is fully orthogonal to the relocation/rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_INSTSUBSTITUTIONPASS_H
#define NEVERD_PASS_IR_INSTSUBSTITUTIONPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Replaces integer add/sub/and/or/xor with equivalent instruction sequences.
struct InstSubstitutionPass : public llvm::PassInfoMixin<InstSubstitutionPass> {
  explicit InstSubstitutionPass(unsigned Rounds = 1) : Rounds(Rounds) {}

  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors HelloWorldPass::inject): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of instructions that were substituted.
  static unsigned inject(llvm::Module &M, unsigned Rounds = 1);

  unsigned Rounds;
};

} // namespace neverd

#endif // NEVERD_PASS_IR_INSTSUBSTITUTIONPASS_H
