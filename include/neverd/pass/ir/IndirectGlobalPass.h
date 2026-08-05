//===- IndirectGlobalPass.h - Indirect global-variable obfuscation -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) indirect global-variable pass.  For every *direct* reference
/// to a defined global variable `@g` inside a function it materialises `@g`'s
/// address as a PC-relative `ptrtoint(@g)`, launders it through a volatile
/// stack slot, and feeds the resulting opaque pointer back in place of `@g`.
/// This hides global data references from a static disassembler (a
/// load/store/GEP no longer names `@g` directly) while keeping exact program
/// semantics.  It is the ninth demo-level sample transform and the *data dual*
/// of IndirectCallPass: IndirectCallPass hides the call graph (direct `call @g`
/// -> indirect call), this pass hides global *data* references (`@g` operand ->
/// indirect address).
///
/// Why `ptrtoint(@g)` rather than an absolute global-pointer table: taking a
/// defined global's address lowers PC-relatively (AArch64 ADRP+ADD, x86-64
/// RIP-relative, ARM PC-relative), so the construct carries no absolute data
/// pointer that would need a load-time rebase under ASLR/PIE — the same
/// reasoning as IndirectCallPass's PIC address.  The volatile slot defeats the
/// backend's tendency to fold `inttoptr(ptrtoint(@g))` straight back into a
/// direct `@g` reference (which would erase the obfuscation), so the rewrite
/// reaches the emitted machine code.
///
/// The rewrite is exactly semantics-preserving: the routed pointer always
/// equals `@g`, so every load/store/GEP touches the same memory.  A pure IR
/// transform fully orthogonal to the relocation/rewrite backend (L1): it scopes to defined
/// globals so the materialised address is an in-image, PC-relative reference
/// the address model resolves without a GOT, and runs after symbolization,
/// before codegen.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_INDIRECTGLOBALPASS_H
#define NEVERD_PASS_IR_INDIRECTGLOBALPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Converts direct references to defined global variables into
/// position-independent indirect address materializations.
struct IndirectGlobalPass : public llvm::PassInfoMixin<IndirectGlobalPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of global-variable references made indirect.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_INDIRECTGLOBALPASS_H
