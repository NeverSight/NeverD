//===- MedLLVMDeadSlots.h - Private SSA storage cleanup -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve observable source state while removing redundant private storage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDEADSLOTS_H
#define NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDEADSLOTS_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class AllocaInst;
class StoreInst;
} // namespace llvm

namespace neverd {
/// Remove unused emitter-private slots and the pure computations feeding them
/// after implicit ABI uses and native exception edges have been materialized.
/// Source memory operations, escaping slots and live values remain intact.
void discardDeadMedLLVMSlots(llvm::ArrayRef<llvm::AllocaInst *> Slots);

/// Remove an emitter-private entry seed only when the slot's sole later store
/// dominates every read. Keeping that redundant store makes mem2reg compute a
/// dominance frontier for each otherwise single-definition SSA temporary.
/// Escaping slots and reads that can still observe the seed remain unchanged.
/// All seeds must belong to the same completed function.
void discardOverwrittenMedLLVMSeeds(llvm::ArrayRef<llvm::StoreInst *> Seeds);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDEADSLOTS_H
