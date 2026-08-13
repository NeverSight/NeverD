//===- MedLLVMEmitter.h - MedIR to LLVM IR emitter -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Umbrella header for the MedIR to LLVM IR emitter.  Every caller includes
/// this file; it pulls in the pieces the emitter interface is split across so
/// no include site has to know which one declares what.
///
///   - MedLLVMEmitterState.h -- support types for the emitter's per-function
///     state (deferred dispatch stores, sub-register propagation entries, the
///     stack-slot cache key).
///   - MedLLVMEmitterCore.h  -- the MedLLVMEmitter class itself, plus the map
///     of which translation unit implements which group of its members.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_MEDLLVMEMITTER_H
#define NEVERD_BACKEND_LLVM_MEDLLVMEMITTER_H

#include "neverd/backend/llvm/MedLLVMEmitterCore.h"
#include "neverd/backend/llvm/MedLLVMEmitterState.h"

#endif // NEVERD_BACKEND_LLVM_MEDLLVMEMITTER_H
