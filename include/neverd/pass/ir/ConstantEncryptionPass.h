//===- ConstantEncryptionPass.h - Constant encryption pass ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) constant-encryption pass.  Replaces integer constant
/// operands of binary operators and integer comparisons with a value that is
/// decrypted at run time, so the literal constant no longer appears in the
/// emitted machine code.
///
/// For an eligible constant \c C of width \c W the pass emits, once per width
/// in the function entry block, an *opaque* copy \c k of a fixed key \c K via a
/// volatile stack slot (a volatile store followed by a volatile load — neither
/// can be eliminated nor value-forwarded, so the backend cannot fold it back to
/// a constant).  Each use of \c C is then rewritten to <tt>(C ^ K) ^ k</tt>,
/// where <tt>C ^ K</tt> is folded at compile time and <tt>^ k</tt> survives to
/// machine code; at run time <tt>k == K</tt> so the value is exactly \c C.
///
/// This is a deliberately small, demo-level sample transform — the second L1
/// sample after InstSubstitutionPass — showing how a constant-obfuscation pass
/// plugs into the patch pipeline.  More substantial transforms will be added as
/// separate passes later.
///
/// It is a pure IR transform: it produces ordinary machine code (stack access +
/// xor) with ordinary fixups, so it is fully orthogonal to the relocation /
/// rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_CONSTANTENCRYPTIONPASS_H
#define NEVERD_PASS_IR_CONSTANTENCRYPTIONPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Encrypts integer constant operands of binary operators / comparisons,
/// decrypting them at run time through an opaque key.
struct ConstantEncryptionPass
    : public llvm::PassInfoMixin<ConstantEncryptionPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors InstSubstitutionPass::inject): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of constant operands that were encrypted.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_CONSTANTENCRYPTIONPASS_H
