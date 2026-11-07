//===- X64StateObjectPointers.h - Explicit state object access -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain producer-owned state and event object pointers in LLVM C source.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64STATEOBJECTPOINTERS_H
#define NEVERD_LIB_PIPELINE_X86_X64STATEOBJECTPOINTERS_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace llvm {
class Module;
}

namespace neverd {

/// Only for modules built by the explicit x64 continuation producer: argument
/// zero of every definition points at its complete StateBytes-byte object.
/// Promote compiler scaffolding, then project constant, wholly contained state
/// loads/stores back to that pointer. Guest addresses and ordinary source
/// parameters have no such object contract and must not use this operation.
/// EventFunction, when supplied by that producer, additionally declares its
/// argument one as a complete X64ContinuationEventV1 input object.
void preserveX64StateObjectPointers(llvm::Module &Module, uint64_t StateBytes,
                                    llvm::StringRef EventFunction = {});

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64STATEOBJECTPOINTERS_H
