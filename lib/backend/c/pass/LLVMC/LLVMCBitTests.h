//===- LLVMCBitTests.h - Project a tested bit to its source -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded bit-condition normalization on the C emitter's private IR clone.

#ifndef NEVERD_LIB_BACKEND_C_PASS_LLVMC_BITTESTS_H
#define NEVERD_LIB_BACKEND_C_PASS_LLVMC_BITTESTS_H

#include "neverd/backend/Limits.h"

namespace llvm {
class Function;
}

namespace neverd::llvmc {
// Retains complete word users and existing effectful producers. Exhaustion
// leaves the remaining tests unchanged; it does not establish a bit value.
bool simplifyBitTests(
    llvm::Function &Function,
    unsigned WorkLimit = ::neverd::limits::backend::kLLVMCBitTestsWorkLimit);
/// Reuse the bit proof for narrow integer reads. Unread register fragments
/// disappear before C evaluates an unavailable enclosing word.
bool simplifyIntegerSlices(
    llvm::Function &Function,
    unsigned WorkLimit = ::neverd::limits::backend::kLLVMCBitTestsWorkLimit);
} // namespace neverd::llvmc

#endif
