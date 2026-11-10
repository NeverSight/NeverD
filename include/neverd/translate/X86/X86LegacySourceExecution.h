//===- X86LegacySourceExecution.h - Protected source bridge ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original-byte C dispatch over the existing protected execution owner.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86LEGACYSOURCEEXECUTION_H
#define NEVERD_TRANSLATE_X86_X86LEGACYSOURCEEXECUTION_H

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>

namespace neverd::translate {

/// Execution statuses retain X86LegacyExecutionStatus values. Driver-only
/// exits occupy a separate range, not x64 continuation's status namespace.
enum class X86LegacySourceExit : uint32_t {
  InvalidState = 256,
  Unselected,
  BudgetExhausted,
  PendingEvent,
};

/// A linear instruction address and its CS.D decode mode. CS base and EIP
/// remain live state: aliases and relative transfers use the current caches.
inline uint64_t x86LegacySourceKey(uint32_t Address, bool Code32) {
  return uint64_t(Address) | uint64_t(Code32) << 32;
}

/// Internal C++ embedding contract, not a stable SDK state layout. The caller
/// retains State, Memory, options' borrowed device/debug callbacks and their
/// contexts across every pause. One CPU, exclusive ordinary RAM while a step
/// executes. The generated driver owns only selection and instruction budget;
/// all fetch, instruction, debug, event and partial-progress semantics remain
/// in X86LegacyExecution. External events are explicitly delivered by the host
/// between driver calls. No initial state, event producer or C ABI is inferred.
class X86LegacySourceExecution final {
public:
  X86LegacySourceExecution(X86LegacyExecutionState &State,
                           X86PagedMemory &Memory,
                           const X86LegacyExecutionOptions &Options)
      : State(State), Memory(Memory), Options(Options) {}

  /// No guest accesses. Pending delivery and a halted CPU block selection.
  /// Selection and waiting preserve the previous instruction receipt for
  /// inspection; neither starts an instruction or spends its budget.
  uint32_t select(uint64_t &Key);
  /// A retained instruction whose attempt receipt was already published may
  /// resume at zero budget. No accesses, validation or event progress here.
  bool pendingAttemptConsumed() const;
  uint32_t step(uint64_t Key, llvm::ArrayRef<uint8_t> Bytes);
  uint32_t deliver(const X86LegacyEvent &Event);
  uint32_t resume();

  const X86LegacyExecutionResult &lastResult() const { return Last; }
  llvm::StringRef lastError() const { return Error; }

private:
  uint64_t key() const;
  uint32_t record(llvm::Expected<X86LegacyExecutionResult> Result);
  uint32_t fail(llvm::Error Failure);
  X86LegacyExecutionState &State;
  X86PagedMemory &Memory;
  X86LegacyExecutionOptions Options;
  X86LegacyExecutionResult Last;
  std::string Error;
};

/// Private generated-source bridge; Context borrows X86LegacySourceExecution.
/// Link against the matching NeverD source build, not the stable SDK ABI.
extern "C" uint32_t neverd_x86_legacy_select_v1(void *Context, uint64_t *Key);
extern "C" uint32_t neverd_x86_legacy_step_v1(void *Context, uint64_t Key,
                                              const uint8_t *Bytes,
                                              uint32_t Size);
extern "C" uint32_t neverd_x86_legacy_attempted_v1(void *Context);
extern "C" uint32_t neverd_x86_legacy_pending_v1(void *Context);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86LEGACYSOURCEEXECUTION_H
