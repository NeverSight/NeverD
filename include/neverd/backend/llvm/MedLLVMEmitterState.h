//===- MedLLVMEmitterState.h - MedLLVM emitter state types ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Support types for the per-function state MedLLVMEmitter carries: the
/// deferred computed-goto dispatch store, the sub-register propagation entry,
/// and the stack-slot key the address-predicate caches are keyed by.  Split
/// out of MedLLVMEmitter.h, which remains the umbrella header every caller
/// includes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_MEDLLVMEMITTERSTATE_H
#define NEVERD_BACKEND_LLVM_MEDLLVMEMITTERSTATE_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::med_llvm {

/// Deferred stores for a shared -O0 computed-goto dispatch recovered as a
/// switch: each predecessor goto-site computes its own index, so the index is
/// communicated to the dispatch block through a common stack slot.  The
/// dispatch loads the slot for its switch; here we record (slot,
/// per-predecessor index) so that, after all blocks are emitted, a store of
/// each predecessor's index into the slot is inserted before that
/// predecessor's terminator (the alloca model makes this dominance-correct
/// without a PHI / critical-edge split).
struct PendingDispatchStore {
  llvm::AllocaInst *Slot = nullptr;
  llvm::Type *Ty = nullptr;
  std::vector<std::pair<int, MedVar>> Preds; ///< (predecessor block id, index)
};

/// One narrower alloca that a wide register write must also update, as
/// recorded in MedLLVMEmitter's sub-register propagation map.
struct SubRegAllocInfo {
  std::pair<int, int> Key;
  llvm::AllocaInst *Alloca;
  unsigned Bits;
};

/// Identity of a constant stack slot in the addrSlotKey form
/// `{(base Id, SSAVer), byte offset}`, used to key the per-function stack-slot
/// address-predicate caches.
using SlotKey = std::pair<std::pair<int, int>, int64_t>;

} // namespace neverd::med_llvm

#endif // NEVERD_BACKEND_LLVM_MEDLLVMEMITTERSTATE_H
