//===- X86LegacyExecutionInternal.h - Legacy admission ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared state admission for instruction execution and IDT delivery.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYEXECUTIONINTERNAL_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYEXECUTIONINTERNAL_H

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/Support/Error.h"

namespace neverd::translate::detail {

llvm::Error admitLegacyExecution(const X86LegacyExecutionState &State,
                                 X86PagedMemory &Memory,
                                 const X86LegacyExecutionOptions &Options);

/// Authenticated return, with staged register/cache state and retained RAM
/// effects and a private transfer journal. The caller publishes retained
/// progress and NMI unblocking even when the staged register image is rejected.
llvm::Expected<X86MemoryAccessResult>
returnLegacyFrame(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                  unsigned Bytes, uint16_t Discard, bool Interrupt,
                  llvm::ArrayRef<uint8_t> Instruction,
                  const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYEXECUTIONINTERNAL_H
