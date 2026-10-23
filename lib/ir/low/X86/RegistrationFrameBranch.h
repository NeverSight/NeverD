//===- RegistrationFrameBranch.h - Exact frame CFG choices ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share constant-condition and complete-edge checks across frame proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONFRAMEBRANCH_H
#define NEVERD_IR_LOW_X86_REGISTRATIONFRAMEBRANCH_H

#include "neverd/Common.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>

namespace neverd {
struct LowBlock;

namespace registration_state {
class FrameTransfer;

/// BlockIdAt must name the current graph's exact entry, or no value. Both
/// the encoded target and the physical fallthrough must be actual edges.
std::optional<va_t>
knownFrameBranchTarget(const LowBlock &Block, const FrameTransfer &Transfer,
                       llvm::function_ref<std::optional<int>(va_t)> BlockIdAt);

} // namespace registration_state
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONFRAMEBRANCH_H
