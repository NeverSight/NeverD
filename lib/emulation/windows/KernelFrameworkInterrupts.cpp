//===- KernelFrameworkInterrupts.cpp - KMDF interrupt lifetimes -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Framework object and power ownership around the shared WDM interrupt
/// authority. Interrupt locks, resource assignment and delivery stay in WDM.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "KernelResources.h"
#include "KernelScheduler.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}

bool isInterruptAPI(llvm::StringRef Name) {
  return Name == api::WdfInterruptCreate ||
         Name == api::WdfInterruptQueueDpcForIsr ||
         Name == api::WdfInterruptQueueWorkItemForIsr ||
         Name == api::WdfInterruptSynchronize ||
         Name == api::WdfInterruptAcquireLock ||
         Name == api::WdfInterruptReleaseLock ||
         Name == api::WdfInterruptEnable || Name == api::WdfInterruptDisable ||
         Name == api::WdfInterruptWdmGetInterrupt ||
         Name == api::WdfInterruptGetInfo || Name == api::WdfInterruptGetDevice;
}
} // namespace

llvm::Expected<uint64_t>
KernelFramework::createInterrupt(Binding &B, llvm::ArrayRef<uint64_t> A) {
  auto Device = Devices.find(A[1]);
  auto DeviceObject = Objects.find(A[1]);
  if (Device == Devices.end() || DeviceObject == Objects.end() ||
      DeviceObject->second.Binding != B.Globals ||
      DeviceObject->second.Deleting)
    return invalid("interrupt creation requires a live framework device");
  auto &D = Device->second;
  if (!D.PDO)
    return invalid("control devices cannot own framework interrupts");
  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != InterruptConfigSize)
    return InfoLengthMismatch;
  if (auto E = ValidateAccess(A[2], InterruptConfigSize, false))
    return E;
  // Read every field before allocating or publishing an object.
  auto SpinLock = read(A[2] + InterruptSpinLock);
  auto Share = read(A[2] + InterruptShareVector, 4);
  auto Floating = read(A[2] + InterruptFloatingSave, 1);
  auto Automatic = read(A[2] + InterruptAutomaticSerialization, 1);
  auto ISR = read(A[2] + InterruptISR);
  auto DPC = read(A[2] + InterruptDPC);
  auto Enable = read(A[2] + InterruptEnable);
  auto Disable = read(A[2] + InterruptDisable);
  auto WorkItem = read(A[2] + InterruptWorkItem);
  auto Raw = read(A[2] + InterruptRaw);
  auto Translated = read(A[2] + InterruptTranslated);
  auto WaitLock = read(A[2] + InterruptWaitLock);
  auto Passive = read(A[2] + InterruptPassiveHandling, 1);
  auto Inactive = read(A[2] + InterruptReportInactive, 4);
  auto Wake = read(A[2] + InterruptCanWake, 1);
  llvm::Error Errors = llvm::Error::success();
  for (auto *Field :
       {&SpinLock, &Share, &Floating, &Automatic, &ISR, &DPC, &Enable, &Disable,
        &WorkItem, &Raw, &Translated, &WaitLock, &Passive, &Inactive, &Wake})
    if (!*Field)
      Errors = llvm::joinErrors(std::move(Errors), Field->takeError());
  if (Errors)
    return std::move(Errors);
  if (!*ISR || *Share > InterruptTriDefault ||
      *Inactive > InterruptTriDefault || (*DPC && *WorkItem) ||
      (*Passive && *SpinLock) || (!*Passive && *WaitLock))
    return InvalidParameter;
  if (*SpinLock || *WaitLock)
    return invalid("external framework interrupt lock objects are not modeled");
  if (*Wake || *Inactive == InterruptTriTrue)
    return invalid("wake-armed and retained inactive interrupt connections are "
                   "not modeled");
  if (*Automatic && *DPC)
    return IncompatibleExecutionLevel;
  if (*Automatic)
    return invalid("automatic parent callback serialization is not modeled");
  // FloatingSave is ignored by Windows on x64; register state is preserved by
  // the execution backend for both ordinary and interrupt callbacks.
  auto Validation = attributes(A[3], AttributesUse::Object);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  auto Attrs = std::get<Attributes>(*Validation);
  if (Attrs.Parent && Attrs.Parent != A[1]) {
    auto Queue = Queues.find(Attrs.Parent);
    if (Queue != Queues.end() && Queue->second.Device == A[1])
      return invalid("interrupt queue parents require automatic callback "
                     "serialization, which is not modeled");
    return ParentAssignmentNotAllowed;
  }
  Attrs.Parent = A[1];

  const bool Preparing = std::any_of(
      PnpTransitions.begin(), PnpTransitions.end(), [&](const auto &Entry) {
        return Entry.second.Device == A[1] &&
               Entry.second.Current.Phase == PnpPhase::PrepareHardware &&
               !Entry.second.CallbacksComplete;
      });
  if ((Preparing && (!*Raw || !*Translated)) ||
      (!Preparing && (*Raw || *Translated || D.HardwarePrepared || D.InD0)))
    return InvalidDeviceState;
  Interrupt Item;
  Item.Device = A[1];
  Item.AssociatedObject = Attrs.Parent;
  Item.ISR = *ISR;
  Item.DPC = *DPC;
  Item.WorkItem = *WorkItem;
  Item.Enable = *Enable;
  Item.Disable = *Disable;
  Item.Selection.PDO = D.PDO;
  Item.Selection.Passive = bool(*Passive);
  if (*Share != InterruptTriDefault)
    Item.Selection.ShareVector = *Share == InterruptTriTrue;
  Item.Selection.Ordinal = std::count_if(
      InterruptObjects.begin(), InterruptObjects.end(),
      [&](const auto &Entry) { return Entry.second.Device == A[1]; });
  if (Preparing) {
    std::optional<uint32_t> Index;
    uint32_t InterruptIndex = 0;
    for (uint32_t I = 0; I < D.RawResources.Count; ++I) {
      const uint64_t Offset = I * resources::ResourceDescriptorSize;
      auto Type = read(D.RawResources.Descriptors + Offset, 1);
      if (!Type)
        return Type.takeError();
      if (D.RawResources.Descriptors + Offset == *Raw &&
          D.TranslatedResources.Descriptors + Offset == *Translated &&
          *Type == resources::InterruptType)
        Index = InterruptIndex;
      if (*Type == resources::InterruptType)
        ++InterruptIndex;
    }
    if (!Index)
      return InvalidParameter;
    Item.Selection.ResourceIndex = *Index;
    Item.Selection.MessageOrdinal =
        std::count_if(InterruptObjects.begin(), InterruptObjects.end(),
                      [&](const auto &Entry) {
                        return Entry.second.Device == A[1] &&
                               Entry.second.Selection.ResourceIndex == Index;
                      });
  }
  if (!InterruptsHost.Connect || !InterruptsHost.Describe ||
      !InterruptsHost.Disconnect || !InterruptsHost.PrepareCall)
    return invalid("framework interrupt host is unavailable");
  if (auto E = writable(A[4], 8))
    return E;
  if (auto E = Memory.writeInteger(A[4], 0, 8))
    return E;
  auto Handle = createObject(B.Globals, Attrs, false);
  if (!Handle)
    return Handle.takeError();
  Objects.at(*Handle).Kind = ObjectKind::Interrupt;
  InterruptObjects.emplace(*Handle, std::move(Item));
  if (auto E = Memory.writeInteger(A[4], *Handle, 8))
    return E;
  return windows::StatusSuccess;
}

llvm::Error
KernelFramework::prepareInterruptCall(uint64_t Token, uint64_t Handle,
                                      uint64_t Routine,
                                      llvm::ArrayRef<uint64_t> Arguments) {
  auto I = InterruptObjects.find(Handle);
  if (I == InterruptObjects.end() || !I->second.Connection || PendingCall ||
      !InterruptsHost.PrepareCall || !Routine)
    return invalid("interrupt callback lost its connection or continuation");
  auto Execution = InterruptsHost.PrepareCall(I->second.Connection->Token,
                                              Routine, Arguments, Token);
  if (!Execution)
    return Execution.takeError();
  PendingCall = GuestCall{Token, Routine, Arguments.vec(), *Execution};
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callInterrupt(llvm::StringRef Name, Binding &B,
                               llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  if (!isInterruptAPI(Name))
    return std::optional<uint64_t>{};
  if (Name == api::WdfInterruptCreate || Name == api::WdfInterruptEnable ||
      Name == api::WdfInterruptDisable) {
    if (IRQL != scheduler::PassiveLevel)
      return invalid(
          "interrupt creation and power callbacks require PASSIVE_LEVEL");
  }
  if (Name == api::WdfInterruptCreate) {
    auto Result = createInterrupt(B, A);
    if (!Result)
      return Result.takeError();
    return std::optional<uint64_t>{*Result};
  }
  auto Object = Objects.find(A[1]);
  auto Entry = InterruptObjects.find(A[1]);
  if (Object == Objects.end() || Entry == InterruptObjects.end() ||
      Object->second.Binding != B.Globals || Object->second.Deleting)
    return invalid(
        "interrupt operation requires a live matching framework interrupt");
  auto &I = Entry->second;
  if (Name == api::WdfInterruptGetDevice)
    return std::optional<uint64_t>{I.Device};
  if (Name == api::WdfInterruptWdmGetInterrupt)
    return std::optional<uint64_t>{I.Connection ? I.Connection->Token : 0};
  if (Name == api::WdfInterruptGetInfo) {
    if (IRQL > scheduler::DispatchLevel)
      return invalid("WdfInterruptGetInfo requires IRQL <= DISPATCH_LEVEL");
    auto Size = read(A[2], 4);
    if (!Size)
      return Size.takeError();
    if (*Size != InterruptInfoSize)
      return invalid("invalid WDF_INTERRUPT_INFO size");
    if (auto E = writable(A[2], InterruptInfoSize))
      return E;
    if (!Devices.at(I.Device).ResourcesActive)
      return invalid(
          "interrupt information requires assigned hardware resources");
    auto Information = InterruptsHost.Describe(I.Selection);
    if (!Information)
      return Information.takeError();
    const auto Info = Information->value_or(InterruptConnection{});
    std::vector<uint8_t> Empty(InterruptInfoSize);
    if (auto E = Memory.write(A[2], Empty))
      return E;
    struct Field {
      uint64_t Offset, Value;
      unsigned Width;
    };
    for (const auto &F :
         {Field{0, InterruptInfoSize, 4},
          Field{InterruptInfoAffinity, Info.Affinity, 8},
          Field{InterruptInfoMessage, Info.MessageID, 4},
          Field{InterruptInfoVector, Info.Vector, 4},
          Field{InterruptInfoIRQL, Info.IRQL, 1},
          Field{InterruptInfoMode, Info.Mode, 4},
          Field{InterruptInfoPolarity, Info.Polarity, 4},
          Field{InterruptInfoMessageSignaled, Info.Message, 1},
          Field{InterruptInfoShare,
                Info.Share ? uint64_t(DriverInterruptShare::Shared)
                           : resources::DeviceExclusive,
                1}})
      if (auto E = Memory.writeInteger(A[2] + F.Offset, F.Value, F.Width))
        return E;
    return std::optional<uint64_t>{0};
  }
  if (Name == api::WdfInterruptQueueDpcForIsr ||
      Name == api::WdfInterruptQueueWorkItemForIsr) {
    const bool WorkItem = Name == api::WdfInterruptQueueWorkItemForIsr;
    const uint64_t Routine = WorkItem ? I.WorkItem : I.DPC;
    if (!Routine || !InterruptsHost.QueueDeferred)
      return invalid("interrupt has no requested deferred callback");
    if (!I.Connection || !Devices.at(I.Device).ResourcesActive)
      return invalid(
          "deferred interrupt callback requires assigned connected resources");
    if (NextContinuation == UINT64_MAX ||
        Object->second.InternalReferences == UINT64_MAX)
      return invalid(
          "interrupt callback identity or reference count exhausted");
    const uint64_t Token = NextContinuation;
    auto Queued = InterruptsHost.QueueDeferred(A[1], I.Selection.PDO, Routine,
                                               {A[1], I.AssociatedObject},
                                               WorkItem, Token);
    if (!Queued)
      return Queued.takeError();
    if (*Queued) {
      ++NextContinuation;
      ++Object->second.InternalReferences;
      InterruptContinuations.emplace(
          Token, InterruptContinuation{A[1], InterruptCallKind::Deferred});
    }
    return std::optional<uint64_t>{*Queued};
  }
  if (Name == api::WdfInterruptAcquireLock ||
      Name == api::WdfInterruptReleaseLock) {
    if (!I.Connection)
      return invalid("interrupt lock requires a connected interrupt");
    const auto &Operation = Name == api::WdfInterruptAcquireLock
                                ? InterruptsHost.Acquire
                                : InterruptsHost.Release;
    if (!Operation)
      return invalid("interrupt lock host is unavailable");
    if (auto E = Operation(I.Connection->Token))
      return E;
    return std::optional<uint64_t>{0};
  }
  const bool Synchronize = Name == api::WdfInterruptSynchronize;
  const bool Enabling = Name == api::WdfInterruptEnable;
  if (!I.Connection)
    return invalid("interrupt callback requires a connected interrupt");
  if (I.ChangingState || PendingCall)
    return invalid("interrupt state change is already pending");
  if (!Synchronize && I.Enabled == Enabling)
    return std::optional<uint64_t>{0};
  const uint64_t Routine = Synchronize ? A[2] : Enabling ? I.Enable : I.Disable;
  if (!Routine) {
    if (Synchronize)
      return invalid("interrupt synchronization requires a callback");
    I.Enabled = Enabling;
    return std::optional<uint64_t>{0};
  }
  if (NextContinuation == UINT64_MAX)
    return invalid("interrupt callback identity exhausted");
  const uint64_t Token = NextContinuation;
  if (auto E = prepareInterruptCall(Token, A[1], Routine,
                                    {A[1], Synchronize ? A[3] : I.Device}))
    return E;
  ++NextContinuation;
  I.ChangingState = !Synchronize;
  ++Object->second.InternalReferences;
  InterruptContinuations.emplace(
      Token,
      InterruptContinuation{A[1], Synchronize ? InterruptCallKind::Synchronize
                                  : Enabling  ? InterruptCallKind::Enable
                                              : InterruptCallKind::Disable});
  return std::optional<uint64_t>{0};
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::finishInterruptCallback(uint64_t Token, uint64_t Result) {
  auto Continuation = InterruptContinuations.find(Token);
  if (Continuation == InterruptContinuations.end())
    return invalid("unknown framework interrupt continuation");
  const auto State = Continuation->second;
  auto Object = Objects.find(State.Object);
  auto Interrupt = InterruptObjects.find(State.Object);
  if (Object == Objects.end() || Interrupt == InterruptObjects.end() ||
      !Object->second.InternalReferences)
    return invalid("interrupt callback lost its retained object");
  uint64_t ReturnValue = 0;
  if (State.Kind == InterruptCallKind::Synchronize)
    ReturnValue = uint8_t(Result);
  else if (State.Kind != InterruptCallKind::Deferred) {
    Interrupt->second.ChangingState = false;
    if (uint32_t(Result) & profile::NTStatusFailureMask)
      return invalid("explicit interrupt enable or disable callback failed");
    if (uint32_t(Result) == windows::StatusPending)
      return invalid("interrupt callback returned STATUS_PENDING");
    Interrupt->second.Enabled = State.Kind == InterruptCallKind::Enable;
  }
  --Object->second.InternalReferences;
  InterruptContinuations.erase(Continuation);
  if (auto E = resumePausedPnp())
    return E;
  return PendingCall ? std::optional<uint64_t>{}
                     : std::optional<uint64_t>{ReturnValue};
}

bool KernelFramework::hasDeferredInterrupts(uint64_t Device) const {
  return std::any_of(InterruptObjects.begin(), InterruptObjects.end(),
                     [&](const auto &Entry) {
                       return Entry.second.Device == Device &&
                              (Objects.at(Entry.first).InternalReferences ||
                               (InterruptsHost.HasDeferred &&
                                InterruptsHost.HasDeferred(Entry.first)));
                     });
}

llvm::Error KernelFramework::disconnectInterrupts(uint64_t Device) {
  for (auto &[Handle, I] : InterruptObjects) {
    if (I.Device != Device || !I.Connection)
      continue;
    if (I.ChangingState)
      return invalid("interrupt disconnect overlaps a state callback");
    if (auto E = InterruptsHost.Disconnect(I.Connection->Token))
      return E;
    I.Connection.reset();
    I.Enabled = false;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> KernelFramework::advancePnpInterrupts(uint64_t Token,
                                                           bool Enable) {
  auto &Transition = PnpTransitions.at(Token);
  for (auto &[Handle, I] : InterruptObjects) {
    if (I.Device != Transition.Device || Handle <= Transition.CurrentInterrupt)
      continue;
    Transition.CurrentInterrupt = Handle;
    if (Enable && !I.Connection) {
      auto Connection = InterruptsHost.Connect(I.Selection, Handle, I.ISR);
      if (!Connection)
        return Connection.takeError();
      I.Connection = *Connection;
    }
    if (!I.Connection || I.Enabled == Enable)
      continue;
    const uint64_t Routine = Enable ? I.Enable : I.Disable;
    if (!Routine) {
      I.Enabled = Enable;
      continue;
    }
    if (auto E =
            prepareInterruptCall(Token, Handle, Routine, {Handle, I.Device}))
      return E;
    I.ChangingState = true;
    return true;
  }
  Transition.CurrentInterrupt = 0;
  if (!Enable)
    if (auto E = disconnectInterrupts(Transition.Device))
      return E;
  return false;
}

llvm::Error KernelFramework::finishPnpInterrupt(uint64_t Token,
                                                uint32_t Status) {
  auto &Transition = PnpTransitions.at(Token);
  auto Entry = InterruptObjects.find(Transition.CurrentInterrupt);
  if (Entry == InterruptObjects.end() || !Entry->second.ChangingState)
    return invalid("power interrupt callback lost its state transition");
  auto &I = Entry->second;
  I.ChangingState = false;
  if (!(Status & profile::NTStatusFailureMask))
    I.Enabled = Transition.Current.Phase == PnpPhase::EnableInterrupts;
  return llvm::Error::success();
}
} // namespace neverd::emulation
