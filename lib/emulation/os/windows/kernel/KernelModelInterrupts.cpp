//===- KernelModelInterrupts.cpp - Guest interrupt synchronization -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest callbacks enter their interrupt lock only after the session saves
/// the caller context. Completion restores the same execution state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error apiError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "interrupt API: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::acquireInterruptSpinLock(uint64_t Object) {
  auto Old = Interrupts.acquire(Object, CurrentExecution, CurrentIRQL);
  if (!Old)
    return Old.takeError();
  CurrentIRQL = Interrupts.connection(Object)->SynchronizeIRQL;
  return *Old;
}

llvm::Expected<uint64_t>
KernelModel::releaseInterruptSpinLock(uint64_t Object, uint8_t OldIRQL) {
  auto Restored =
      Interrupts.release(Object, CurrentExecution, OldIRQL, CurrentIRQL);
  if (!Restored)
    return Restored.takeError();
  CurrentIRQL = *Restored;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::synchronizeInterrupt(uint64_t Object,
                                                           uint64_t Routine,
                                                           uint64_t Context) {
  if (hasPendingModelGuestCall() || PendingWait)
    return apiError("cannot replace a prepared guest callback");
  const auto *Connection = Interrupts.connection(Object);
  if (!Connection || CurrentIRQL > Connection->SynchronizeIRQL)
    return apiError(
        "synchronization requires a live interrupt at caller IRQL <= DIRQL");
  auto Call = Interrupts.synchronize(Object, Routine, Context);
  if (!Call)
    return Call.takeError();
  if (Connection->Passive) {
    if (PendingWait)
      return apiError("passive synchronization cannot replace a pending wait");
    Wait Waiter;
    Waiter.Type = Wait::Kind::InterruptSynchronization;
    Waiter.Object = Call->Token.ID;
    Waiter.Execution = CurrentExecution;
    Waiter.IRQL = CurrentIRQL;
    PendingWait = Waiter;
  }
  PendingInterruptCall = std::move(*Call);
  return 0;
}

llvm::Error KernelModel::beginGuestCall(GuestCallToken Token) {
  if (!Token.ID)
    return apiError("guest callback has no continuation identity");
  if (Token.Owner == GuestCallOwner::PoFx) {
    const auto *Call = PoFx.callback(Token.ID);
    if (Call && Call->Thread && Call->Thread != CurrentThreadKey)
      return apiError("PoFx callback must enter on its blocking caller thread");
    return PoFx.beginCallback(Token.ID);
  }
  if (Token.Owner == GuestCallOwner::DMA)
    return beginDMACall(Token.ID);
  if (Token.Owner != GuestCallOwner::Interrupt)
    return llvm::Error::success();
  auto IRQL = Interrupts.beginCall(Token.ID, CurrentIRQL, Scheduler.now100ns());
  if (!IRQL)
    return IRQL.takeError();
  CurrentIRQL = *IRQL;
  // Entry is prepared before DriverSession selects the callback's stack and
  // thread. Bind the implicit critical region at its first enterExecution.
  if (*IRQL == scheduler::PassiveLevel)
    EnteringPassiveInterrupt = Token.ID;
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishInterruptCall(uint64_t Token, uint64_t Value) {
  auto Return =
      Interrupts.finishCall(Token, Value, CurrentIRQL, Scheduler.now100ns());
  if (!Return)
    return Return.takeError();
  PassiveInterruptThreads.erase(Token);
  if (EnteringPassiveInterrupt == Token)
    EnteringPassiveInterrupt.reset();
  CurrentIRQL = Return->RestoredIRQL;
  if (Return->Next) {
    PendingInterruptCall = std::move(*Return->Next);
    return std::optional<uint64_t>{};
  }
  auto FrameworkToken = FrameworkInterruptContinuations.find(Token);
  if (FrameworkToken != FrameworkInterruptContinuations.end()) {
    const uint64_t Continuation = FrameworkToken->second;
    FrameworkInterruptContinuations.erase(FrameworkToken);
    // The interrupt layer interprets only ISR BOOLEAN observations. Framework
    // enable/disable callbacks return NTSTATUS and must retain every bit.
    return finishGuestCall({GuestCallOwner::Framework, Continuation}, Value);
  }
  return std::optional<uint64_t>{Return->Value};
}

llvm::Error KernelModel::validateExecutionReturn(uint64_t Identity,
                                                 uint8_t EntryIRQL,
                                                 bool Nested) const {
  if (Identity != CurrentExecution)
    return apiError("return does not own the active execution identity");
  if (hasProcessAttachment(Identity))
    return apiError("guest return has an unmatched process attachment");
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Lock.Execution == Identity)
      return apiError("guest return retains an executive spin lock");
  for (const auto &[Lock, Execution] : FrameworkWaitLockThreads)
    if (Execution == Identity)
      return apiError("guest return retains a framework wait lock");
  for (const auto &[Owner, Thread] : FrameworkPassiveLockThreads)
    if (Owner.first == Identity)
      return apiError("guest return retains a passive interrupt lock");
  for (const auto &[Key, Entry] : FrameworkCallbackEntries)
    if (Key.first == Identity)
      return apiError("guest return retains a framework callback lock");
  for (const RaisedIRQL &Raise : RaisedIRQLs)
    if (Raise.Execution == Identity)
      return apiError("guest return retains a raised IRQL");
  if (!Nested && Dispatcher.ownsMutex(CurrentThreadKey))
    return apiError(dispatcher::OwnedMutexReturn);
  if (!Nested && SystemAffinityThreads.contains(CurrentThreadKey))
    return apiError("guest return retains an unmatched system affinity");
  if (auto It = ApcStates.find(CurrentThreadKey);
      !Nested && It != ApcStates.end()) {
    const bool SystemThread =
        Scheduler.active() &&
        Scheduler.active()->Kind == KernelScheduler::CallbackKind::SystemThread;
    if (It->second.GuardedDepth ||
        It->second.CriticalDepth > (SystemThread ? 1 : 0))
      return apiError("guest return retains an unmatched APC-disable region");
  }
  if (auto E = Interrupts.validateExecutionReturn(Identity))
    return E;
  if (CancelLock.Callback && CancelLock.CallbackExecution == Identity) {
    if (!CancelLock.Callback || CancelLock.Held ||
        CurrentIRQL != CancelLock.CallbackIRQL)
      return apiError(
          "WDM cancel callback must release the cancel spin lock and "
          "restore its caller IRQL");
    return llvm::Error::success();
  }
  if (CurrentIRQL != EntryIRQL)
    return apiError("guest return did not restore entry IRQL");
  return llvm::Error::success();
}
} // namespace neverd::emulation
