//===- RegistrationFrameBits.h - PE32 register slice dependencies ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove that a register slice discards pointer-dependent bits.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEBITS_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEBITS_H

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <optional>

namespace llvm {
class TruncInst;
class Value;
} // namespace llvm

namespace neverd::coff_registration {
class RegistrationFrameStores;

/// A register slice can discard all pointer-dependent bits of a wider value.
/// Check masks through immutable LLVM operations and unaliased private spills.
/// Unknown operations retain taint; exhausted work has no successful result.
std::optional<bool> truncationMayDependOnFrame(
    const llvm::TruncInst &Trunc,
    llvm::function_ref<bool(const llvm::Value *)> IsAddress,
    const RegistrationFrameStores &Stores, size_t &Work);
} // namespace neverd::coff_registration

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEBITS_H
