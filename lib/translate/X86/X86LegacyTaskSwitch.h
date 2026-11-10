//===- X86LegacyTaskSwitch.h - Protected task publication -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared task-switch publication, backing resume and incoming fault context.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYTASKSWITCH_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYTASKSWITCH_H

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

enum class X86LegacyTaskReason : uint8_t { Call, Jump, Interrupt, Return };

struct X86LegacyTaskRequest {
  X86LegacyTaskReason Reason;
  uint16_t Selector = 0;
  uint32_t SavedEIP = 0;
  uint32_t SavedFlags = 0;
  bool ThroughGate = false;
  bool External = false;
  bool EventOwner = false;
  std::optional<uint32_t> ErrorCode;
  /// A direct far operand has already fetched its TSS descriptor. Retain
  /// that input, including when subsequent accesses alias its storage.
  std::optional<uint64_t> Descriptor;
};

struct X86LegacyTaskResult : X86MemoryAccessResult {
  bool Committed = false;
  bool DebugTrap = false;
  bool ModeUnavailable = false;
};

llvm::Expected<X86LegacyTaskResult>
switchLegacyTask(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                 const X86LegacyTaskRequest &Request,
                 llvm::ArrayRef<uint8_t> Instruction,
                 const X86LegacyExecutionOptions &Options);

llvm::Expected<X86LegacyTaskResult>
resumeLegacyTask(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                 llvm::ArrayRef<uint8_t> Instruction,
                 const X86LegacyExecutionOptions &Options);

bool legacyTaskFaultContext(const X86LegacyExecutionState &State);
bool legacyTaskInstructionPending(const X86LegacyExecutionState &State);
bool legacyTaskEventPending(const X86LegacyExecutionState &State);
bool legacyTaskModeUnavailable(const X86LegacyExecutionState &State);
uint64_t legacyTaskInstructionKey(const X86LegacyExecutionState &State);
uint32_t legacyTaskEntryFlags(const X86LegacyExecutionState &State);
uint64_t legacyTaskEntryDebugControl(const X86LegacyExecutionState &State);
uint8_t legacyTaskWatchpoints(const X86LegacyExecutionState &State);
void retainLegacyTaskWatchpoints(X86LegacyExecutionState &State,
                                 uint8_t Matches);
llvm::Error validateLegacyTaskState(const X86LegacyExecutionState &State,
                                    const X86PagedMemory &Memory);
void completeLegacyTaskFault(X86LegacyExecutionState &State);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYTASKSWITCH_H
