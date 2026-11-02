//===- LLVMCAtomicOrdering.h - C atomic memory orders -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared memory-order spelling for LLVM C atomic operations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVMC_ATOMICORDERING_H
#define NEVERD_BACKEND_C_LLVMC_ATOMICORDERING_H

#include "llvm/Support/AtomicOrdering.h"

#include <stdexcept>

namespace neverd {

inline const char *llvmCAtomicOrderingToken(llvm::AtomicOrdering Ordering) {
  switch (Ordering) {
  case llvm::AtomicOrdering::Unordered:
  case llvm::AtomicOrdering::Monotonic:
    return "__ATOMIC_RELAXED";
  case llvm::AtomicOrdering::Acquire:
    return "__ATOMIC_ACQUIRE";
  case llvm::AtomicOrdering::Release:
    return "__ATOMIC_RELEASE";
  case llvm::AtomicOrdering::AcquireRelease:
    return "__ATOMIC_ACQ_REL";
  case llvm::AtomicOrdering::SequentiallyConsistent:
    return "__ATOMIC_SEQ_CST";
  default:
    throw std::runtime_error("unsupported LLVM C atomic ordering");
  }
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_LLVMC_ATOMICORDERING_H
