//===- KernelDispatcher.cpp - Concrete Windows dispatcher APIs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original implementation of public Microsoft dispatcher contracts:
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-keinsertqueuedpc
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/nf-ntddk-kesetimportancedpc
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kesettimerex
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kecanceltimer
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kesetevent
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kereleasesemaphore
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-keinitializemutex
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kereleasemutex
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kereadstatemutex
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/defining-and-using-an-event-object
/// No host timers, execution of host function pointers, or native structure
/// overlays are used. The scheduler chooses one explicit single-CPU schedule.
///
//===----------------------------------------------------------------------===//

#include "KernelDispatcher.h"

#include "../GuestMemory.h"
#include "KernelException.h"

#include <limits>

namespace neverd::emulation {
namespace {
enum class API {
  Unknown,
#define NEVERD_KERNEL_DISPATCHER_API(Name, Count) Name,
#include "KernelDispatcherAPIs.def"
#undef NEVERD_KERNEL_DISPATCHER_API
};
llvm::Error dispatcherError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

bool validRange(uint64_t Address, uint64_t Size) {
  return Size && Size <= std::numeric_limits<uint64_t>::max() - Address;
}

bool overlap(uint64_t Address, uint64_t Size, uint64_t Other,
             uint64_t OtherSize) {
  return Address < Other + OtherSize && Other < Address + Size;
}
} // namespace

llvm::Error KernelDispatcher::configure(uint64_t NewOwner, uint64_t NewThread) {
  if (Owner || !NewOwner || !NewThread || !Validate)
    return dispatcherError("invalid or repeated dispatcher configuration");
  Owner = NewOwner;
  Thread = NewThread;
  return llvm::Error::success();
}

std::optional<unsigned> KernelDispatcher::argumentCount(llvm::StringRef Name) {
#define NEVERD_KERNEL_DISPATCHER_API(Symbol, Count)                            \
  if (Name == #Symbol)                                                         \
    return Count;
#include "KernelDispatcherAPIs.def"
#undef NEVERD_KERNEL_DISPATCHER_API
  return std::nullopt;
}

bool KernelDispatcher::hasArmedTimerReference(uint64_t DPC) const {
  for (const auto &[Address, State] : Objects)
    if (State.Type == Kind::Timer && State.TimerDPC == DPC &&
        Scheduler.isTimerArmed(Address))
      return true;
  return false;
}

llvm::Error KernelDispatcher::canRelease(uint64_t Address,
                                         const Object &State) const {
  if (State.Type == Kind::Mutex && State.MutexDepth)
    return dispatcherError("cannot release storage with an owned mutex");
  if (State.Type == Kind::DPC) {
    if (Scheduler.isDPCQueued(Address))
      return dispatcherError("dispatcher DPC is still queued");
    if (hasArmedTimerReference(Address))
      return dispatcherError("dispatcher DPC is referenced by an armed timer");
  }
  if (State.Type == Kind::Timer && State.HasSchedule)
    return Scheduler.canForgetTimer(Address);
  return llvm::Error::success();
}

llvm::Error KernelDispatcher::initialize(uint64_t Address, Object State) {
  if (!Address || Address % dispatcher::ObjectAlignment ||
      !validRange(Address, State.Size))
    return dispatcherError("invalid or misaligned dispatcher object range");
  auto Existing = Objects.find(Address);
  if (Existing == Objects.end() && Objects.size() >= dispatcher::MaxObjects)
    return dispatcherError("dispatcher object count exceeds the limit");
  for (const auto &[Other, Entry] : Objects)
    if (Other != Address && overlap(Address, State.Size, Other, Entry.Size))
      return dispatcherError("dispatcher object ranges overlap");
  if (Existing != Objects.end())
    if (auto E = canRelease(Address, Existing->second))
      return E;
  if (auto E = Validate(Address, State.Size, true))
    return E;
  // The bytes are reserved opaque storage, not a serialization of model state.
  if (auto E = Memory.write(Address, std::vector<uint8_t>(State.Size)))
    return E;
  if (Existing != Objects.end() && Existing->second.Type == Kind::Timer &&
      Existing->second.HasSchedule)
    if (auto E = Scheduler.forgetTimer(Address))
      return E;
  Objects.insert_or_assign(Address, std::move(State));
  return llvm::Error::success();
}

llvm::Expected<KernelDispatcher::Object *>
KernelDispatcher::object(uint64_t Address, Kind Type, bool IsWrite) {
  auto It = Objects.find(Address);
  if (It == Objects.end() || It->second.Type != Type)
    return dispatcherError(
        "dispatcher object is uninitialized or has wrong type");
  if (auto E = Validate(Address, It->second.Size, IsWrite))
    return E;
  return &It->second;
}

llvm::Error KernelDispatcher::validateGuestAccess(uint64_t Address,
                                                  uint32_t Size,
                                                  bool IsWrite) const {
  if (!Size)
    return llvm::Error::success();
  if (!validRange(Address, Size))
    return dispatcherError("dispatcher access range overflow");
  for (const auto &[Other, State] : Objects)
    if (overlap(Address, Size, Other, State.Size))
      return dispatcherError(IsWrite
                                 ? "guest write to opaque dispatcher object"
                                 : "guest read from opaque dispatcher object");
  return llvm::Error::success();
}

llvm::Error KernelDispatcher::canReleaseRange(uint64_t Address,
                                              uint64_t Size) const {
  if (!validRange(Address, Size))
    return dispatcherError("invalid dispatcher release range");
  for (const auto &[Other, State] : Objects) {
    if (!overlap(Address, Size, Other, State.Size))
      continue;
    if (Other < Address || Other + State.Size > Address + Size)
      return dispatcherError("release partially overlaps a dispatcher object");
    if (auto E = canRelease(Other, State))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelDispatcher::prepareReleaseRange(uint64_t Address,
                                                  uint64_t Size) {
  if (auto E = canReleaseRange(Address, Size))
    return E;
  std::vector<uint64_t> Releasing;
  for (const auto &[Other, State] : Objects)
    if (overlap(Address, Size, Other, State.Size))
      Releasing.push_back(Other);
  for (uint64_t Other : Releasing) {
    const Object &State = Objects.at(Other);
    if (State.Type == Kind::Timer && State.HasSchedule)
      if (auto E = Scheduler.forgetTimer(Other))
        return E;
    Objects.erase(Other);
  }
  return llvm::Error::success();
}

bool KernelDispatcher::isWaitable(uint64_t Address) const {
  auto It = Objects.find(Address);
  return It != Objects.end() && It->second.Type != Kind::DPC;
}

bool KernelDispatcher::ownsMutex(uint64_t Execution) const {
  if (!Execution)
    return false;
  for (const auto &[Address, State] : Objects)
    if (State.Type == Kind::Mutex && State.MutexDepth &&
        State.MutexOwner == Execution)
      return true;
  return false;
}

llvm::Expected<bool> KernelDispatcher::signaled(uint64_t Address,
                                                const Object &State) const {
  if (State.Type == Kind::Mutex)
    return State.MutexDepth == 0;
  if (State.Type == Kind::Semaphore)
    return State.Count > 0;
  if (State.Type == Kind::Timer)
    return State.HasSchedule ? Scheduler.timerSignaled(Address)
                             : llvm::Expected<bool>(false);
  return State.Signaled;
}

llvm::Expected<bool> KernelDispatcher::tryAcquire(uint64_t Address,
                                                  uint64_t Execution,
                                                  uint8_t CurrentIRQL) {
  if (!isWaitable(Address))
    return dispatcherError("unsupported or uninitialized wait object");
  Object &State = Objects.at(Address);
  if (auto E = Validate(Address, State.Size, false))
    return E;
  if (auto E = Scheduler.processDueTimers())
    return E;
  if (State.Type == Kind::Mutex) {
    if (!Execution)
      return dispatcherError("mutex wait requires an active execution");
    if (State.MutexDepth && State.MutexOwner != Execution)
      return false;
    if (State.MutexDepth == uint32_t(std::numeric_limits<int32_t>::max()) + 1)
      return llvm::make_error<KernelGuestException>(
          dispatcher::StatusMutantLimitExceeded);
    if (!State.MutexDepth) {
      State.MutexOwner = Execution;
      State.MutexAcquiredAtDispatch = CurrentIRQL == dispatcher::DispatchLevel;
    } else if (State.MutexAcquiredAtDispatch !=
               (CurrentIRQL == dispatcher::DispatchLevel)) {
      return dispatcherError("recursive mutex wait changed DISPATCH_LEVEL");
    }
    ++State.MutexDepth;
    return true;
  }
  auto Signal = signaled(Address, State);
  if (Signal && *Signal && State.Type == Kind::Semaphore) {
    --State.Count;
    return true;
  }
  if (!Signal || !*Signal || !State.Synchronization)
    return Signal;
  if (State.Type == Kind::Timer)
    return Scheduler.consumeTimerSignal(Address);
  State.Signaled = false;
  return true;
}

llvm::Expected<uint64_t> KernelDispatcher::call(llvm::StringRef Name,
                                                llvm::ArrayRef<uint64_t> Args,
                                                uint8_t CurrentIRQL,
                                                uint64_t Execution) {
  const auto Count = argumentCount(Name);
  if (!Count || Args.size() != *Count)
    return dispatcherError("unknown dispatcher API or invalid argument count");
  if (!Owner)
    return dispatcherError("dispatcher has not been configured");
  API Function = API::Unknown;
#define NEVERD_KERNEL_DISPATCHER_API(Symbol, Count)                            \
  if (Name == #Symbol)                                                         \
    Function = API::Symbol;
#include "KernelDispatcherAPIs.def"
#undef NEVERD_KERNEL_DISPATCHER_API
  const bool AnyIRQL = Function == API::KeInitializeDpc ||
                       Function == API::KeInsertQueueDpc ||
                       Function == API::KeRemoveQueueDpc ||
                       Function == API::KeSetImportanceDpc ||
                       Function == API::KeSetTargetProcessorDpc ||
                       Function == API::KeInitializeEvent ||
                       Function == API::KeReadStateSemaphore ||
                       Function == API::KeInitializeMutex;
  if (CurrentIRQL >
      (AnyIRQL ? dispatcher::HighLevel : dispatcher::DispatchLevel))
    return dispatcherError("dispatcher API called at unsupported IRQL");

  switch (Function) {
  case API::KeInitializeDpc: {
    if (!Args[1])
      return dispatcherError("DPC callback address is null");
    Object State{Kind::DPC, dispatcher::DpcSize};
    State.DPC.Object = Args[0];
    State.DPC.Owner = Owner;
    State.DPC.Thread = Thread;
    State.DPC.PC = Args[1];
    State.DPC.Arguments = {Args[0], Args[2], 0, 0};
    if (auto E = initialize(Args[0], std::move(State)))
      return E;
    return 0;
  }
  case API::KeInitializeTimer:
  case API::KeInitializeTimerEx:
  case API::KeInitializeEvent: {
    const bool Event = Function == API::KeInitializeEvent;
    const uint32_t Type = Function == API::KeInitializeTimer
                              ? dispatcher::NotificationObject
                              : static_cast<uint32_t>(Args[1]);
    if (Type > dispatcher::SynchronizationObject)
      return dispatcherError("unsupported dispatcher event or timer type");
    Object State{Event ? Kind::Event : Kind::Timer,
                 static_cast<uint32_t>(Event ? dispatcher::EventSize
                                             : dispatcher::TimerSize)};
    State.Synchronization = Type == dispatcher::SynchronizationObject;
    State.Signaled = Event && static_cast<uint8_t>(Args[2]);
    if (auto E = initialize(Args[0], std::move(State)))
      return E;
    return 0;
  }
  case API::KeInitializeSemaphore: {
    const int32_t Count = static_cast<int32_t>(Args[1]);
    const int32_t Limit = static_cast<int32_t>(Args[2]);
    if (CurrentIRQL != scheduler::PassiveLevel)
      return dispatcherError("semaphore initialization requires PASSIVE_LEVEL");
    if (Count < 0 || Limit <= 0 || Count > Limit ||
        Args[1] != static_cast<uint32_t>(Count) ||
        Args[2] != static_cast<uint32_t>(Limit))
      return dispatcherError("invalid initial semaphore count or limit");
    Object State{Kind::Semaphore, dispatcher::SemaphoreSize};
    State.Count = Count;
    State.Limit = Limit;
    if (auto E = initialize(Args[0], std::move(State)))
      return E;
    return 0;
  }
  case API::KeInitializeMutex: {
    if (Args[1])
      return dispatcherError("KeInitializeMutex Level must be zero");
    Object State{Kind::Mutex, dispatcher::MutexSize};
    if (auto E = initialize(Args[0], std::move(State)))
      return E;
    return 0;
  }
  case API::KeInsertQueueDpc:
  case API::KeRemoveQueueDpc:
  case API::KeSetImportanceDpc:
  case API::KeSetTargetProcessorDpc: {
    auto State = object(Args[0], Kind::DPC, true);
    if (!State)
      return State.takeError();
    if (Function == API::KeInsertQueueDpc) {
      auto DPC = (*State)->DPC;
      DPC.Arguments[2] = Args[1];
      DPC.Arguments[3] = Args[2];
      auto Queued = Scheduler.queueDPC(std::move(DPC));
      if (!Queued)
        return Queued.takeError();
      return *Queued;
    }
    if (Function == API::KeRemoveQueueDpc)
      return Scheduler.removeDPC(Args[0]);
    if (Function == API::KeSetTargetProcessorDpc) {
      if (static_cast<uint8_t>(Args[1]))
        return dispatcherError("only processor zero is modeled for DPCs");
      return 0;
    }
    const uint32_t Importance = static_cast<uint32_t>(Args[1]);
    if (Importance >
        static_cast<uint32_t>(KernelScheduler::DpcImportance::MediumHigh))
      return dispatcherError("invalid DPC importance");
    if (hasArmedTimerReference(Args[0]))
      return dispatcherError(
          "DPC importance changes with an armed timer are unsupported");
    (*State)->DPC.Importance =
        static_cast<KernelScheduler::DpcImportance>(Importance);
    return 0;
  }
  case API::KeSetTimer:
  case API::KeSetTimerEx:
  case API::KeCancelTimer:
  case API::KeReadStateTimer: {
    auto State =
        object(Args[0], Kind::Timer, Function != API::KeReadStateTimer);
    if (!State)
      return State.takeError();
    if (Function == API::KeReadStateTimer) {
      if (auto E = Scheduler.processDueTimers())
        return E;
      auto Signal = signaled(Args[0], **State);
      if (!Signal)
        return Signal.takeError();
      return *Signal;
    }
    if (Function == API::KeCancelTimer)
      return Scheduler.cancelTimer(Args[0]);
    const bool Extended = Function == API::KeSetTimerEx;
    const uint32_t Period = Extended ? static_cast<uint32_t>(Args[2]) : 0;
    if (Period > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
      return dispatcherError("timer period must be a nonnegative LONG");
    const uint64_t DPCAddress = Args[Extended ? 3 : 2];
    std::optional<KernelScheduler::DpcCallback> DPC;
    if (DPCAddress) {
      auto DPCState = object(DPCAddress, Kind::DPC, true);
      if (!DPCState)
        return DPCState.takeError();
      DPC = (*DPCState)->DPC;
      // CustomTimerDpc SystemArgument1/2 are unspecified; this concrete
      // schedule supplies zero. They never contain host addresses.
    }
    auto Armed = Scheduler.setTimer(
        Args[0], Owner, static_cast<int64_t>(Args[1]), Period, std::move(DPC));
    if (!Armed)
      return Armed.takeError();
    (*State)->HasSchedule = true;
    (*State)->TimerDPC = DPCAddress;
    if (auto E = Scheduler.processDueTimers())
      return E;
    return *Armed;
  }
  case API::KeSetEvent:
  case API::KeResetEvent:
  case API::KeClearEvent:
  case API::KeReadStateEvent: {
    auto State =
        object(Args[0], Kind::Event, Function != API::KeReadStateEvent);
    if (!State)
      return State.takeError();
    const bool Previous = (*State)->Signaled;
    if (Function == API::KeReadStateEvent)
      return Previous;
    if (Function == API::KeSetEvent) {
      if (static_cast<uint8_t>(Args[2]))
        return dispatcherError(
            "KeSetEvent Wait=TRUE requires unsupported IRQL handoff");
      if (static_cast<uint32_t>(Args[1]))
        return dispatcherError("event priority boosts are unsupported");
      (*State)->Signaled = true;
    } else {
      (*State)->Signaled = false;
    }
    return Function == API::KeClearEvent ? 0 : Previous;
  }
  case API::KeReadStateSemaphore:
  case API::KeReleaseSemaphore: {
    auto State =
        object(Args[0], Kind::Semaphore, Function == API::KeReleaseSemaphore);
    if (!State)
      return State.takeError();
    const int32_t Count = (*State)->Count;
    if (Function == API::KeReadStateSemaphore)
      return Count;
    if (static_cast<uint32_t>(Args[1]))
      return dispatcherError("semaphore priority boosts are unsupported");
    if (static_cast<uint8_t>(Args[3]))
      return dispatcherError(
          "KeReleaseSemaphore Wait=TRUE requires unsupported IRQL handoff");
    const int32_t Adjustment = static_cast<int32_t>(Args[2]);
    if (Adjustment <= 0 || Args[2] != static_cast<uint32_t>(Adjustment))
      return dispatcherError("semaphore adjustment must be positive LONG");
    if (Adjustment > (*State)->Limit - Count)
      return llvm::make_error<KernelGuestException>(
          dispatcher::StatusSemaphoreLimitExceeded);
    (*State)->Count += Adjustment;
    return static_cast<uint32_t>(Count);
  }
  case API::KeReadStateMutex:
  case API::KeReleaseMutex: {
    auto State = object(Args[0], Kind::Mutex, Function == API::KeReleaseMutex);
    if (!State)
      return State.takeError();
    const int32_t Previous =
        static_cast<int32_t>(1 - int64_t((*State)->MutexDepth));
    if (Function == API::KeReadStateMutex)
      return static_cast<uint32_t>(Previous);
    if (static_cast<uint8_t>(Args[1]))
      return dispatcherError(
          "KeReleaseMutex Wait=TRUE requires unsupported IRQL handoff");
    if (!Execution || !(*State)->MutexDepth ||
        (*State)->MutexOwner != Execution)
      return llvm::make_error<KernelGuestException>(
          dispatcher::StatusMutantNotOwned);
    if ((*State)->MutexAcquiredAtDispatch !=
        (CurrentIRQL == dispatcher::DispatchLevel))
      return dispatcherError("mutex release changed DISPATCH_LEVEL");
    if (!--(*State)->MutexDepth) {
      (*State)->MutexOwner = 0;
      (*State)->MutexAcquiredAtDispatch = false;
    }
    return static_cast<uint32_t>(Previous);
  }
  case API::Unknown:
    break;
  }
  return dispatcherError("unhandled dispatcher API");
}

} // namespace neverd::emulation
