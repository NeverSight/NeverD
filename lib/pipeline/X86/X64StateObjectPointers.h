//===- X64StateObjectPointers.h - Explicit state object access -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain the producer-owned state object's pointer in LLVM C source.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64STATEOBJECTPOINTERS_H
#define NEVERD_LIB_PIPELINE_X86_X64STATEOBJECTPOINTERS_H

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
void preserveX64StateObjectPointers(llvm::Module &Module, uint64_t StateBytes);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64STATEOBJECTPOINTERS_H
