//===- X86LegacyTransferJournal.h - Retained transfer RAM -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Private access progress shared by frame returns and VM86 far branches.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYTRANSFERJOURNAL_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYTRANSFERJOURNAL_H

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace neverd::translate::detail {

enum class X86LegacyTransferKind : uint8_t { Far, Return };

/// Each attempt works on a private copy. Only a backing stop publishes its
/// immutable progress; guest faults, completion and owner refusals retire it.
class X86LegacyTransferJournal {
  X86LegacyExecutionState &State;
  std::shared_ptr<X86LegacyTransferContinuation> Progress;
  size_t NextAccess = 0;

public:
  X86LegacyTransferJournal(X86LegacyExecutionState &State,
                           const X86PagedMemory &Memory,
                           const X86LegacyExecutionOptions &Options,
                           llvm::ArrayRef<uint8_t> Instruction,
                           X86LegacyTransferKind Kind, bool Active = true);
  llvm::Expected<bool> replay(uint32_t Address, unsigned Bytes, bool Write,
                              uint64_t &Value);
  void record(uint32_t Address, unsigned Bytes, bool Write, uint64_t Value);
  void finish(X86PageWalkStatus Status);
};

/// Admission errors leave the original continuation intact.
llvm::Error
validateLegacyTransferContinuation(const X86LegacyExecutionState &State,
                                   const X86PagedMemory &Memory,
                                   llvm::ArrayRef<uint8_t> Instruction,
                                   const X86LegacyExecutionOptions &Options);
uint8_t legacyTransferWatchpoints(const X86LegacyExecutionState &State);
void retainLegacyTransferWatchpoints(X86LegacyExecutionState &State,
                                     uint8_t Matches);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYTRANSFERJOURNAL_H
