//===- AArch64State.h - Atomic architectural register capture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_STATE_H
#define NEVERD_EMULATION_AARCH64_STATE_H
#include "AArch64Machine.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::emulation {
using AArch64RegisterReader =
    llvm::function_ref<llvm::Expected<uint64_t>(AArch64Register)>;
using AArch64VectorReader =
    llvm::function_ref<llvm::Expected<RegisterValue>(unsigned)>;
/// Normalize a completely read packet using the architectural inventory.
/// Vectors and configured privilege remain unchanged. Callers must validate
/// every transport read before publishing the packet to architectural state.
void normalizeAArch64State(AArch64MachineState &State);
/// Publish the complete scalar and vector inventory as one transaction. A
/// failed transport read cannot expose an advanced PC or partial FP/SIMD state.
llvm::Error captureAArch64State(AArch64MachineState &State,
                                AArch64RegisterReader ReadScalar,
                                AArch64VectorReader ReadVector);
} // namespace neverd::emulation
#endif
