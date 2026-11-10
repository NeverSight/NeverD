//===- X86LegacyFarTransfer.h - Protected far branches ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict far CALL/JMP decoding and ordinary-RAM call-gate execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYFARTRANSFER_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYFARTRANSFER_H

#include "X86LegacyEncoding.h"
#include "X86LegacyTaskSwitch.h"

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacyFarTransfer {
  X86LegacyAddress Address;
  uint32_t Offset = 0;
  uint16_t Selector = 0;
  unsigned Bytes = 4;
  bool Call = false, Memory = false, Locked = false;
};

std::optional<X86LegacyFarTransfer>
decodeLegacyFarTransfer(llvm::ArrayRef<uint8_t> Bytes, bool Code32);

llvm::Error validateLegacyFarContinuation(
    const X86LegacyExecutionState &State, const X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Bytes, const X86LegacyExecutionOptions &Options);

/// Ordinary far branches stage caches until completion. Hardware task branches
/// publish their incoming state at the task owner's commit point, including
/// when a subsequent descriptor load faults.
llvm::Expected<X86LegacyTaskResult>
executeLegacyFarTransfer(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                         const X86LegacyFarTransfer &Instruction,
                         uint32_t NextIP, llvm::ArrayRef<uint8_t> Bytes,
                         const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYFARTRANSFER_H
