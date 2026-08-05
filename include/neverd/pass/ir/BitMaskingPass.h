//===- BitMaskingPass.h - Bit-masking value obfuscation --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) bit-masking pass.  Replaces selected integer (scalar or
/// integer-vector) instruction results `x` with the bitwise identity
/// `(x & m) | (x & nm)`, where `m` and `nm` are loaded from two *independent*
/// volatile stack slots initialised to a compile-time mask `K` and its true
/// complement `~K`.  At run time `(x & K) | (x & ~K) == x & (K | ~K) == x`
/// for any `K`, so semantics are strictly preserved (element-wise for vectors).
///
/// Because `m` and `nm` come from two separate volatile loads, the backend
/// cannot prove `nm == ~m` and therefore cannot fold the expression back to
/// `x`, so the masking survives to the machine code.  This is the *bitwise*
/// dual of the other value wrappers: ValueLaundering routes a value through a
/// memory round-trip and MBA appends a provably-zero arithmetic term — this one
/// decomposes the value across a mask and its complement and recombines it.
///
/// Deliberately small, demo-level sample transform that shows how an IR pass
/// plugs into the patch pipeline — more substantial transforms will be added as
/// separate passes later.
///
/// It is a pure IR transform: it produces ordinary machine code + ordinary
/// fixups, so it is fully orthogonal to the relocation/rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_BITMASKINGPASS_H
#define NEVERD_PASS_IR_BITMASKINGPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Replaces integer (scalar / integer-vector) instruction results with the
/// bitwise identity `(x & m) | (x & ~m)`, where the two masks come from
/// independent volatile slots so the backend cannot fold the result back.
struct BitMaskingPass : public llvm::PassInfoMixin<BitMaskingPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors HelloWorldPass::inject): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of values that were bit-masked.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_BITMASKINGPASS_H
