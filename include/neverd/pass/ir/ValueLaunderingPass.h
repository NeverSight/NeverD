//===- ValueLaunderingPass.h - Value laundering obfuscation -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) value-laundering pass.  Routes selected integer (scalar or
/// integer-vector) instruction results through a volatile stack slot — a
/// store-volatile / load-volatile round-trip — and redirects the original uses
/// to the reloaded value.  This forces the value out of registers and back
/// through memory, breaking register-level def-use chains.  It is the
/// data-value dual of the indirect-address passes (IndirectCall /
/// IndirectGlobal launder an *address* through a volatile slot to form an
/// opaque pointer; this launders a computed *value*).
///
/// Deliberately small, demo-level sample transform that shows how an IR pass
/// plugs into the patch pipeline — more substantial transforms will be added as
/// separate passes later.
///
/// It is a pure IR transform: it produces ordinary machine code + ordinary
/// fixups, so it is fully orthogonal to the relocation/rewrite backend (L1).  The volatile slot
/// guarantees the round-trip survives to the machine code (the backend cannot
/// elide a volatile load/store), so the value really does detour through the
/// stack while staying bit-for-bit equivalent.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_VALUELAUNDERINGPASS_H
#define NEVERD_PASS_IR_VALUELAUNDERINGPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Routes integer (scalar / integer-vector) instruction results through a
/// volatile stack slot, redirecting their uses to the reloaded value.
struct ValueLaunderingPass : public llvm::PassInfoMixin<ValueLaunderingPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors HelloWorldPass::inject): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of values that were laundered.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_VALUELAUNDERINGPASS_H
