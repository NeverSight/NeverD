//===- COFFRegistrationFrameBits.h - PE32 register slice dependencies ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONFRAMEBITS_H
#define NEVERD_COFFREGISTRATIONFRAMEBITS_H

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

#endif
