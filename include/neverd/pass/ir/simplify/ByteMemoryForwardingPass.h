//===- ByteMemoryForwardingPass.h - Forward overlapping stack bytes -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_SIMPLIFY_BYTEMEMORYFORWARDINGPASS_H
#define NEVERD_PASS_IR_SIMPLIFY_BYTEMEMORYFORWARDINGPASS_H

#include "llvm/IR/PassManager.h"

#include <cstdint>

namespace neverd {

/// Finite per-function limits. Zero permits no work of that kind.
struct ByteMemoryForwardingOptions {
  uint64_t MaxInstructions = 262144;
  uint64_t MaxAddressSteps = 1048576;
  uint64_t MaxTrackedBytes = 65536;
  uint64_t MaxNewInstructions = 262144;
  uint64_t MaxUseSteps = 1048576;
  /// Opt in only when the consumer supports LLVM freeze. The default pipeline
  /// also feeds C emission, which cannot express arbitrary undefined choices.
  bool AllowStoreSnapshots = false;
  uint64_t MaxMemorySteps = 1048576;
  /// Also simplify exact full-width integer addresses, including entry-rooted
  /// PHI/select relations proved at a fixed point and bitwise displacements
  /// certified by dominating masked equalities. This does not assert that
  /// their memory is private or disjoint from other pointers.
  bool SimplifyNumericMemory = false;
};

struct ByteMemoryForwardingResult {
  uint64_t ForwardedLoads = 0;
  uint64_t FrozenStores = 0;
  uint64_t RemovedStores = 0;
  uint64_t CanonicalizedAddresses = 0;
  uint64_t Instructions = 0;
  uint64_t AddressSteps = 0;
  uint64_t PeakTrackedBytes = 0;
  uint64_t NewInstructions = 0;
  uint64_t UseSteps = 0;
  uint64_t MemorySteps = 0;
  bool BudgetExhausted = false;
};

/// Reconstruct complete scalar loads from the last writer of each byte in the
/// same basic block. Only fixed byte allocas and constant, in-object GEPs are
/// accepted. Calls, unknown writes and ordered memory discard the facts.
///
/// With AllowStoreSnapshots, a potentially undefined value is frozen once at
/// its original store; the store and every forwarded fragment share that
/// snapshot. This is LLVM refinement, not evidence of native definedness or
/// argument independence. Otherwise the original load is retained.
/// Stores remain in place for ordinary dead-store elimination to inspect.
/// With SimplifyNumericMemory, integral AS0 inttoptr addresses with the same
/// SSA root and full-width constant offsets also permit single-writer scalar
/// forwarding and removal of completely overwritten, unobserved stores.
/// Backward byte liveness combines later writes and preserves overlapping
/// observations; a disjoint read with the same root does not kill the proof.
/// Address equality may cross a loop only when every incoming value retains
/// the same modular offset from one function-entry value. Memory contents
/// remain block-local; this is not a private-frame or definedness proof.
struct ByteMemoryForwardingPass
    : public llvm::PassInfoMixin<ByteMemoryForwardingPass> {
  explicit ByteMemoryForwardingPass(ByteMemoryForwardingOptions Options = {})
      : Options(Options) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);

  static ByteMemoryForwardingResult
  forward(llvm::Function &F, ByteMemoryForwardingOptions Options = {});

private:
  ByteMemoryForwardingOptions Options;
};

} // namespace neverd

#endif // NEVERD_PASS_IR_SIMPLIFY_BYTEMEMORYFORWARDINGPASS_H
