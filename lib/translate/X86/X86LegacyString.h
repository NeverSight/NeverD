//===- X86LegacyString.h - Protected string progress ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict string grammar and restartable single-element execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYSTRING_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYSTRING_H

#include "X86LegacyScalar.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacyStringInstruction {
  X86LegacyScalarInstruction Element;
  unsigned AddressBytes = 4;
  bool SourceIndex = false, DestinationIndex = false;
  uint8_t Repeat = 0;
  bool Locked = false;
};

std::optional<X86LegacyStringInstruction>
decodeLegacyString(llvm::ArrayRef<uint8_t> Bytes, bool Code32);

struct X86LegacyStringResult : X86MemoryAccessResult {
  bool RepeatContinue = false;
};

/// Reuses scalar operand permissions, alignment ordering and arithmetic.
/// The caller stages architectural state; completed RAM effects are retained.
llvm::Expected<X86LegacyStringResult>
executeLegacyString(X86LegacyExecutionState &Staged, X86PagedMemory &Memory,
                    const X86LegacyStringInstruction &Instruction,
                    uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYSTRING_H
