//===- X86LegacySourceExecution.cpp - Protected source bridge ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain the legacy owner's state and progress across generated C pauses.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LegacySourceExecution.h"

#include "X86LegacyExecutionInternal.h"
#include "X86LegacyTaskSwitch.h"

#include "llvm/Support/Errc.h"

#include <utility>

namespace neverd::translate {

uint64_t X86LegacySourceExecution::key() const {
  if (detail::legacyTaskInstructionPending(State))
    return detail::legacyTaskInstructionKey(State);
  const auto &CS = State.Core.Segments[1];
  return x86LegacySourceKey(uint32_t(CS.Base + State.EIP),
                            bool(CS.Attributes & 0x4000));
}

uint32_t X86LegacySourceExecution::fail(llvm::Error Failure) {
  Last = {};
  Error = llvm::toString(std::move(Failure));
  return uint32_t(X86LegacySourceExit::InvalidState);
}

uint32_t X86LegacySourceExecution::record(
    llvm::Expected<X86LegacyExecutionResult> Result) {
  if (!Result)
    return fail(Result.takeError());
  Error.clear();
  Last = std::move(*Result);
  return uint32_t(Last.Status);
}

uint32_t X86LegacySourceExecution::select(uint64_t &Key) {
  if (detail::legacyTaskModeUnavailable(State) &&
      Options.Virtual8086 != X86LegacyVM86Profile::IntelVME) {
    if (auto Failure = detail::validateLegacyTaskState(State, Memory))
      return fail(std::move(Failure));
    return uint32_t(X86LegacyExecutionStatus::ModeUnavailable);
  }
  if (State.PendingEvent)
    return uint32_t(X86LegacySourceExit::PendingEvent);
  if (auto Failure = detail::admitLegacyExecution(State, Memory, Options))
    return fail(std::move(Failure));
  if (State.Halted) {
    Error.clear();
    return uint32_t(X86LegacyExecutionStatus::Halted);
  }
  Key = key();
  Error.clear();
  return uint32_t(X86LegacyExecutionStatus::Completed);
}

bool X86LegacySourceExecution::pendingAttemptConsumed() const {
  return detail::legacyInstructionPending(State) &&
         State.PendingInstructionAttempted;
}

uint32_t X86LegacySourceExecution::step(uint64_t Key,
                                        llvm::ArrayRef<uint8_t> Bytes) {
  Last = {};
  Error.clear();
  if (State.PendingEvent)
    return uint32_t(X86LegacySourceExit::PendingEvent);
  if (Key != key())
    return fail(llvm::createStringError(
        llvm::errc::invalid_argument,
        "legacy source instruction address or CS.D mode does not match"));
  // The instruction owner validates the complete live state before accesses.
  return record(executeX86LegacyInstruction(State, Memory, Bytes, Options));
}

uint32_t X86LegacySourceExecution::deliver(const X86LegacyEvent &Event) {
  return record(deliverX86LegacyEvent(State, Memory, Event, Options));
}

uint32_t X86LegacySourceExecution::resume() {
  return record(resumeX86LegacyEvent(State, Memory, Options));
}

extern "C" uint32_t neverd_x86_legacy_select_v1(void *Context, uint64_t *Key) {
  if (!Context || !Key)
    return uint32_t(X86LegacySourceExit::InvalidState);
  return static_cast<X86LegacySourceExecution *>(Context)->select(*Key);
}

extern "C" uint32_t neverd_x86_legacy_step_v1(void *Context, uint64_t Key,
                                              const uint8_t *Bytes,
                                              uint32_t Size) {
  if (!Context)
    return uint32_t(X86LegacySourceExit::InvalidState);
  return static_cast<X86LegacySourceExecution *>(Context)->step(
      Key, Bytes ? llvm::ArrayRef(Bytes, Size) : llvm::ArrayRef<uint8_t>{});
}

extern "C" uint32_t neverd_x86_legacy_attempted_v1(void *Context) {
  return Context && static_cast<X86LegacySourceExecution *>(Context)
                        ->lastResult()
                        .InstructionAttempted;
}

extern "C" uint32_t neverd_x86_legacy_pending_v1(void *Context) {
  return Context && static_cast<X86LegacySourceExecution *>(Context)
                        ->pendingAttemptConsumed();
}

} // namespace neverd::translate
