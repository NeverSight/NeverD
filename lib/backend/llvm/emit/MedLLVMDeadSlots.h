//===- MedLLVMDeadSlots.h - Discard unobserved private SSA slots
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDEADSLOTS_H
#define NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDEADSLOTS_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class AllocaInst;
}

namespace neverd {
/// Remove unused emitter-private slots and the pure computations feeding them
/// after implicit ABI uses and native exception edges have been materialized.
/// Source memory operations, escaping slots and live values remain intact.
void discardDeadMedLLVMSlots(llvm::ArrayRef<llvm::AllocaInst *> Slots);
} // namespace neverd

#endif
