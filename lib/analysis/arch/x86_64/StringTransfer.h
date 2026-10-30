//===- StringTransfer.h - Bounded repeated memory transfers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Bounded scalar expansion of ordered x64 repeated memory transfers.

#ifndef NEVERD_ANALYSIS_STRING_TRANSFER_H
#define NEVERD_ANALYSIS_STRING_TRANSFER_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <optional>

namespace neverd::analysis::detail {

struct StringTransferShape {
  unsigned ElementBytes;
  unsigned CountInput;
  bool Fill;
  bool CompleteState = false;

  unsigned elementOperations(bool RuntimeDirection) const {
    return (Fill ? 2 : 4) + RuntimeDirection;
  }

  unsigned fixedOperations(uint64_t Count, bool RuntimeDirection) const {
    const unsigned Completion =
        CompleteState
            ? (Fill ? 2 : 3) + (Fill && ElementBytes != 8 && Count != 0)
            : 1;
    // Normalize the entry direction once, then select the final pointer delta.
    return Completion +
           (RuntimeDirection && Count ? (CompleteState ? 3 : 1) : 0);
  }
};

/// Only exact 64-bit-address MOVS/STOS shapes are admitted. Complete-state
/// operations return the final pointers and count; legacy operations retain
/// their discarded zero result and separate lifter updates. Ordered and
/// segmented accesses cannot become ordinary scalar memory operations.
std::optional<StringTransferShape> stringTransferShape(const LowOp &Op);

/// Lower a caller-proved count. A missing direction retains the operation's
/// runtime direction operand. Each element is read completely
/// before its write, and later iterations observe prior writes. This is not
/// memcpy/memmove. Zero count accesses no memory and ignores Backward.
///
/// All instruction temporaries must appear in InstructionOps. Scratch is
/// allocated after them and below TemporaryLimit. MaxOperations is checked
/// before allocation; the caller must still charge every emitted operation to
/// its shared budget. This routine proves no value, alias, or frame assumption.
llvm::Expected<std::vector<LowOp>>
lowerStringTransfer(const LowOp &Op, uint64_t Count,
                    std::optional<bool> Backward,
                    llvm::ArrayRef<LowOp> InstructionOps,
                    uint64_t TemporaryLimit, uint64_t MaxOperations);

} // namespace neverd::analysis::detail

#endif
