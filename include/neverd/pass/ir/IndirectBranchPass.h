//===- IndirectBranchPass.h - Indirect branch obfuscation ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) indirect-branch pass.  For every two-way conditional branch
/// `br i1 %c, %T, %F` it `select`s between the two successors' block addresses,
/// `select i1 %c, ptr blockaddress(@f,%T), ptr blockaddress(@f,%F)`, launders
/// the chosen target through a volatile stack slot, and replaces the branch
/// with `indirectbr`.  This is the sixth demo-level sample transform (after
/// instruction substitution, constant encryption, opaque predicates,
/// control-flow flattening and bogus control flow).
///
/// Why a `select` of two blockaddresses rather than a global pointer table:
/// each `blockaddress` lowers to a *position-independent* reference to a .text
/// label (AArch64 ADRP+ADD, x86-64 RIP-relative, ARM PC-relative), so the
/// construct survives ASLR/PIE without any load-time pointer rebase.  The
/// volatile slot defeats the backend's tendency to fold a one-of-two indirect
/// branch back into a direct conditional branch (which would erase the
/// obfuscation).
///
/// The rewrite is exactly semantics-preserving: the address selected by the
/// condition is the original taken successor, and `indirectbr` keeps the
/// original block as predecessor so successor PHIs remain valid.  It is a pure
/// IR transform fully orthogonal to the relocation/rewrite backend (L1).
///
/// Backend coverage: validated for semantic equivalence on all 12 ISA ×
/// object-format cells.  The Thumb cell (COFF ARM32) was a gap until the
/// LLVM-fork fix that preserves the Thumb literal-pool PC-anchor in
/// AddressModelBackend::evaluateFixup (the earlier "missing interworking bit"
/// theory was a misdiagnosis — Thumb `mov pc,<reg>` is non-interworking
/// BranchWritePC, so the even blockaddress matches stable LLVM 22).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_INDIRECTBRANCHPASS_H
#define NEVERD_PASS_IR_INDIRECTBRANCHPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Converts two-way conditional branches into position-independent indirect
/// branches.
struct IndirectBranchPass : public llvm::PassInfoMixin<IndirectBranchPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of conditional branches converted.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_INDIRECTBRANCHPASS_H
