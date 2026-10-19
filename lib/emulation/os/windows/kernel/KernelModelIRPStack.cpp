//===- KernelModelIRPStack.cpp - WDM dispatch and completion -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// WDM stack cursors and guest continuations share the original packet. A
/// dispatch return, a completion callback return and IoStatus are distinct.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error stackError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "WDM IRP stack: " + Message);
}

constexpr uint64_t CompletionControlMask =
    StackPendingReturned | StackInvokeOnCancel | StackInvokeOnSuccess |
    StackInvokeOnError;

uint32_t requestMajor(DriverRequestKind Kind) {
  switch (Kind) {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  case DriverRequestKind::Name:                                                \
    return Major;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  }
  return profile::MajorFunctionCount;
}
} // namespace

llvm::Expected<uint32_t> KernelModel::requestStackCursor(uint64_t IRP) const {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return stackError("cursor requires a live owned IRP");
  auto Count = Memory.readInteger(IRP + IRPStackCountOffset, 1);
  auto Location = Memory.readInteger(IRP + IRPLocationOffset, 1);
  auto Pointer = Memory.readInteger(IRP + IRPStackPointerOffset, 8);
  if (!Count || !Location || !Pointer)
    return llvm::joinErrors(
        Count.takeError(),
        llvm::joinErrors(Location.takeError(), Pointer.takeError()));
  if (*Count != Request->StackCount || !*Location ||
      *Location > uint64_t(Request->StackCount) + 1 ||
      *Pointer != IRP + IRPSize + (*Location - 1) * StackSize)
    return stackError("inconsistent or out-of-bounds stack cursor");
  // The one-past slot is a valid cursor after Skip or the final pop, never
  // valid storage for IO_STACK_LOCATION fields.
  return static_cast<uint32_t>(*Location - 1);
}

llvm::Expected<uint64_t> KernelModel::currentRequestStack(uint64_t IRP) const {
  auto Cursor = requestStackCursor(IRP);
  if (!Cursor)
    return Cursor.takeError();
  if (*Cursor == requestForIRP(IRP)->StackCount)
    return stackError("one-past stack cursor cannot be dereferenced");
  return IRP + IRPSize + *Cursor * StackSize;
}

llvm::Expected<bool> KernelModel::dispatchPending(const ActiveRequest &Request,
                                                  uint32_t Slot) const {
  if (Slot >= Request.StackCount)
    return stackError("dispatch lost its stack slot");
  if (Request.UnwoundPending[Slot])
    return *Request.UnwoundPending[Slot];
  if (Request.Completed)
    return stackError("completed dispatch lost its pending observation");
  auto Control = Memory.readInteger(
      Request.IRP + IRPSize + Slot * StackSize + StackControlOffset, 1);
  if (!Control)
    return Control.takeError();
  return (*Control & StackPendingReturned) != 0;
}

llvm::Error KernelModel::validateCompletionPending(const ActiveRequest &Request,
                                                   bool Pending) const {
  if (Request.DispatchReturned &&
      Result.Requests[Request.ResultIndex].DispatchStatus == StatusPending &&
      !Pending)
    return stackError(
        "pending dispatch completion requires propagation to the top stack");
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::callDriver(uint64_t Device, uint64_t IRP, ForwardingOwner Owner,
                        std::optional<int64_t> SendTimeout) {
  if (auto D = Devices.find(Device);
      D != Devices.end() &&
      D->second.OwnerKind == DeviceOwnerKind::FiniteFileSystem)
    return stackError("filesystem endpoint has no provider IRP contract");
  if (!requestForIRP(IRP) && DriverIRPs.contains(IRP)) {
    if (Owner != ForwardingOwner::WDM)
      return stackError("caller-allocated packets require WDM dispatch");
    if (auto E = adoptDriverIRP(Device, IRP))
      return E;
  }
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return stackError("IoCallDriver requires a live owned IRP");
  if (Request->Kind == DriverRequestKind::Pnp &&
      CurrentIRQL >= scheduler::DispatchLevel)
    return stackError("PnP forwarding requires IRQL below DISPATCH_LEVEL");
  if (Request->Kind == DriverRequestKind::Power &&
      CurrentIRQL != scheduler::PassiveLevel)
    return stackError("pageable power forwarding requires PASSIVE_LEVEL");
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return stackError("cannot replace a pending guest callback");
  if (Owner == ForwardingOwner::WDM &&
      (FrameworkUsbIdleRequests.contains(IRP) ||
       (Framework && Framework->ownsRequestIRP(IRP))))
    return stackError("framework-owned requests cannot use WDM forwarding");
  if (!Devices.count(Device) ||
      std::find(Request->DeviceRoute.begin(), Request->DeviceRoute.end(),
                Device) == Request->DeviceRoute.end())
    return stackError("target is outside the request's retained device route");
  if (auto E = validateFileSystemRoute(Request->DeviceRoute,
                                       Request->FileAddress, Request->Kind))
    return E;
  auto Cursor = requestStackCursor(IRP);
  if (!Cursor)
    return Cursor.takeError();
  if (!*Cursor)
    return stackError("IoCallDriver exhausted the IRP stack");
  const uint32_t Slot = *Cursor - 1;
  const uint64_t Stack = IRP + IRPSize + Slot * StackSize;
  auto Major = Memory.readInteger(Stack, 1);
  auto Control = Memory.readInteger(Stack + StackControlOffset, 1);
  auto Completion = Memory.readInteger(Stack + StackCompletionOffset, 8);
  if (!Major || !Control || !Completion)
    return llvm::joinErrors(
        Major.takeError(),
        llvm::joinErrors(Control.takeError(), Completion.takeError()));
  if (*Major >= profile::MajorFunctionCount)
    return stackError("invalid forwarded major function");
  if (*Control & ~CompletionControlMask)
    return stackError("unsupported forwarded stack control flags");
  if ((*Control & ~StackPendingReturned) && !*Completion)
    return stackError("completion invocation flags require a callback");
  // This first stack contract forwards the original operation. Redirected
  // majors require their own packet/transfer construction rules.
  if (*Major != requestMajor(Request->Kind))
    return stackError(
        "changing the request major while forwarding is unsupported");
  if (Request->PowerOperation) {
    auto Minor = Memory.readInteger(Stack + StackMinorOffset, 1);
    if (!Minor)
      return Minor.takeError();
    if (*Minor != uint8_t(Request->PowerOperation->Minor))
      return stackError(
          "changing the power minor while forwarding is unsupported");
  }
  const bool FileProvider = FileSystem.ownsDevice(Device);
  const bool Provider = isPnpProviderDevice(Device) || FileProvider;
  llvm::Expected<uint64_t> PC =
      Provider ? llvm::Expected<uint64_t>(0)
               : guestDriverDispatch(Devices.at(Device).OwnerDriver, *Major);
  if (!PC)
    return PC.takeError();
  if (!Provider && !*PC) {
    if (anyInitialized(DispatchBytesWritten, DispatchInitialization, *Major * 8,
                       8))
      return stackError(
          "driver explicitly registered a null dispatch callback");
  }
  if (NextIRPCall == UINT64_MAX)
    return stackError("continuation identity exhausted");
  auto OldDevice = Memory.readInteger(Stack + StackDeviceOffset, 8);
  if (!OldDevice)
    return OldDevice.takeError();
  const bool WasForwarded = Request->Forwarded;
  const auto WasPending = Request->UnwoundPending[Slot];
  const bool FileBusReceived = Request->FileBusReceived;
  const bool FileSystemReceived = Request->FileSystemReceived;
  const bool UsbReceived = Result.Requests[Request->ResultIndex].UsbIdle &&
                           Result.Requests[Request->ResultIndex]
                               .UsbIdle->BusReceivedAt100ns.has_value();
  const size_t ResultIndex = Request->ResultIndex;
  const auto &Observation = Result.Requests[ResultIndex];
  const auto Received = Observation.Pnp ? Observation.Pnp->BusReceivedAt100ns
                        : Observation.Power
                            ? Observation.Power->BusReceivedAt100ns
                            : std::optional<uint64_t>{};
  if (auto E = Memory.writeInteger(IRP + IRPLocationOffset, Slot + 1, 1))
    return std::move(E);
  if (auto E = Memory.writeInteger(IRP + IRPStackPointerOffset, Stack, 8))
    return std::move(E);
  if (auto E = Memory.writeInteger(Stack + StackDeviceOffset, Device, 8))
    return std::move(E);
  Request->Forwarded = true;
  Request->UnwoundPending[Slot].reset();
  if (Provider) {
    auto Status = FileProvider
                      ? dispatchFileSystem(Device, IRP)
                      : callProviderDriver(Device, IRP, Owner, SendTimeout);
    if (Status) {
      auto Allocation = DriverIRPs.find(IRP);
      if (Allocation != DriverIRPs.end() &&
          !Allocation->second.InitialDispatchToken &&
          !requestForIRP(IRP)->DispatchReturned) {
        if (*Status == StatusPending || requestForIRP(IRP)->Completed) {
          if (auto E = recordDriverIRPDispatchReturn(IRP, uint32_t(*Status)))
            return E;
        } else {
          // Synchronous provider completion can enter the allocating caller
          // before this API returns. Keep its result until that real unwind.
          Allocation->second.ProviderDispatchReturn = uint32_t(*Status);
        }
      }
      return Status;
    }
    auto E = Status.takeError();
    auto *Retained = requestForIRP(IRP);
    const auto &Current = Result.Requests[ResultIndex];
    const auto NowReceived = Current.Pnp     ? Current.Pnp->BusReceivedAt100ns
                             : Current.Power ? Current.Power->BusReceivedAt100ns
                                             : std::optional<uint64_t>{};
    // Provider preflight reads the prospective stack cursor. If it rejected
    // the call without accepting the packet, restore that cursor and its slot
    // metadata. Once a real receipt occurred, effects cannot be rolled back.
    if (Retained && !Retained->Completed && NowReceived == Received &&
        Retained->FileBusReceived == FileBusReceived &&
        Retained->FileSystemReceived == FileSystemReceived &&
        bool(Current.UsbIdle && Current.UsbIdle->BusReceivedAt100ns) ==
            UsbReceived) {
      E = llvm::joinErrors(
          std::move(E),
          Memory.writeInteger(IRP + IRPLocationOffset, *Cursor + 1, 1));
      E = llvm::joinErrors(
          std::move(E),
          Memory.writeInteger(IRP + IRPStackPointerOffset,
                              IRP + IRPSize + *Cursor * StackSize, 8));
      E = llvm::joinErrors(
          std::move(E),
          Memory.writeInteger(Stack + StackDeviceOffset, *OldDevice, 8));
      Retained->Forwarded = WasForwarded;
      Retained->UnwoundPending[Slot] = WasPending;
    }
    return E;
  }
  if (!*PC) {
    if (auto E =
            Memory.writeInteger(IRP + IRPStatusOffset, InvalidDeviceRequest, 4))
      return std::move(E);
    if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, 0, 8))
      return std::move(E);
    Request->IOStatusWritten.fill(true);
    if (auto E = completeRequest(IRP, 0))
      return std::move(E);
    // A default dispatch is synchronous, but its completion can invoke guest
    // callbacks. Its API return must remain STATUS_INVALID_DEVICE_REQUEST.
    if (PendingWdmCall) {
      auto &Call = IRPCalls.at(PendingWdmCall->Token.ID);
      Call.ReturnValue = InvalidDeviceRequest;
    }
    return InvalidDeviceRequest;
  }
  const uint64_t Token = NextIRPCall++;
  IRPCalls.emplace(Token, IRPCall{IRPCallKind::Dispatch, IRP, Slot, true});
  if (auto Allocation = DriverIRPs.find(IRP);
      Allocation != DriverIRPs.end() &&
      !Allocation->second.InitialDispatchToken)
    Allocation->second.InitialDispatchToken = Token;
  PendingWdmCall =
      KernelGuestCall{{GuestCallOwner::WDM, Token}, *PC, {Device, IRP}};
  return 0;
}

std::optional<KernelGuestCall> KernelModel::takeWdmGuestCall() {
  auto Call = std::move(PendingWdmCall);
  PendingWdmCall.reset();
  return Call;
}

llvm::Error KernelModel::completeRequest(uint64_t IRP, uint8_t PriorityBoost) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return stackError(
        "completion requires the active IRP and cannot occur twice");
  if (ProviderCompletions.count(IRP))
    return stackError("provider still owns the pending IRP");
  if (PriorityBoost)
    return stackError("modeled completion supports IO_NO_INCREMENT only");
  if (CancelLock.Held)
    return stackError(
        "IoCompleteRequest cannot run with the cancel spin lock held");
  auto CancelRoutine = Memory.readInteger(IRP + IRPCancelRoutineOffset, 8);
  if (!CancelRoutine)
    return CancelRoutine.takeError();
  if (*CancelRoutine)
    return stackError(
        "IoCompleteRequest requires the cancel routine to be unregistered");
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return stackError("cannot replace a pending guest callback");
  for (unsigned I = 0; I < Request->IOStatusWritten.size(); ++I)
    if ((I < 4 || I >= 8) && !initialized(Request->IOStatusWritten,
                                          Request->IOStatusInitialization, I))
      return stackError(
          "completion requires initialized IoStatus.Status and Information");
  auto Status = Memory.readInteger(IRP + IRPStatusOffset, 4);
  auto Information = Memory.readInteger(IRP + IRPInformationOffset, 8);
  if (!Status || !Information)
    return llvm::joinErrors(Status.takeError(), Information.takeError());
  if (uint32_t(*Status) == StatusPending)
    return stackError("IoCompleteRequest cannot complete with STATUS_PENDING");
  if (Request->FrameworkTransitionAwaiting)
    return stackError("framework PnP callback still owns this completion");
  if (!(uint32_t(*Status) & profile::NTStatusFailureMask)) {
    auto Policy = beginFrameworkPowerPolicy(IRP);
    if (!Policy)
      return Policy.takeError();
    if (*Policy)
      return llvm::Error::success();
  }
  if (Framework && Request->PnpOperation &&
      Request->PnpOperation->Minor == DevicePnpRequest::Start &&
      !Request->FrameworkTransitionHandled &&
      !(uint32_t(*Status) & profile::NTStatusFailureMask) &&
      !Request->DeviceRoute.empty() &&
      FrameworkDevices.count(Request->DeviceRoute.front())) {
    auto Deferred = Framework->beginPnpPowerTransition(
        Request->PnpDevice, IRP, Request->PnpOperation->Minor,
        Request->RawResources, Request->TranslatedResources,
        Request->ResourceListSize);
    if (!Deferred)
      return Deferred.takeError();
    Request->FrameworkTransitionHandled = !*Deferred;
    Request->FrameworkTransitionAwaiting = *Deferred;
    if (*Deferred) {
      if (auto E = markRequestPending(IRP))
        return E;
      return llvm::Error::success();
    }
  }
  if (Framework && Request->PowerOperation &&
      Request->PowerOperation->Minor == DevicePowerRequest::Set &&
      Request->PowerOperation->Type == DriverPowerType::Device &&
      Request->PowerOperation->State == uint32_t(DevicePowerState::D0) &&
      !Request->FrameworkTransitionHandled &&
      !(uint32_t(*Status) & profile::NTStatusFailureMask) &&
      !Request->DeviceRoute.empty() &&
      FrameworkDevices.count(Request->DeviceRoute.front())) {
    const auto &Power = *Result.Requests[Request->ResultIndex].Power;
    auto Deferred = Framework->beginDevicePowerTransition(
        Request->PnpDevice, IRP, Power.DeviceStateBefore,
        DevicePowerState(Power.State));
    if (!Deferred)
      return Deferred.takeError();
    Request->FrameworkTransitionHandled = !*Deferred;
    Request->FrameworkTransitionAwaiting = *Deferred;
    if (*Deferred) {
      if (auto E = markRequestPending(IRP))
        return E;
      return llvm::Error::success();
    }
  }
  auto Cursor = requestStackCursor(IRP);
  if (!Cursor)
    return Cursor.takeError();
  // Invalid terminal ownership must not create a continuation that prevents a
  // corrected packet from completing or finalizing later.
  auto Plan = planIRPCompletion(IRP);
  if (!Plan)
    return Plan.takeError();
  if (NextIRPCall == UINT64_MAX)
    return stackError("continuation identity exhausted");
  const uint64_t Token = NextIRPCall++;
  IRPCalls.emplace(Token, IRPCall{IRPCallKind::Completion, IRP, *Cursor});
  auto Result = advanceIRPCompletion(Token);
  if (!Result)
    return Result.takeError();
  return llvm::Error::success();
}

llvm::Expected<KernelModel::IRPCompletionPlan>
KernelModel::planIRPCompletion(uint64_t IRP,
                               std::optional<uint32_t> StatusOverride,
                               std::optional<bool> CancelOverride) const {
  auto Cursor = requestStackCursor(IRP);
  if (!Cursor)
    return Cursor.takeError();
  const auto &Request = *requestForIRP(IRP);
  llvm::Expected<uint64_t> Status =
      StatusOverride ? llvm::Expected<uint64_t>(*StatusOverride)
                     : Memory.readInteger(IRP + IRPStatusOffset, 4);
  llvm::Expected<uint64_t> Cancel =
      CancelOverride ? llvm::Expected<uint64_t>(*CancelOverride)
                     : Memory.readInteger(IRP + IRPCancelOffset, 1);
  if (!Status || !Cancel)
    return llvm::joinErrors(Status.takeError(), Cancel.takeError());
  if (!StatusOverride && Request.PowerOperation &&
      Request.PowerOperation->Minor == DevicePowerRequest::WaitWake &&
      !(uint32_t(*Status) & profile::NTStatusFailureMask)) {
    const auto &Observation = Result.Requests[Request.ResultIndex].Power;
    if (!Observation || !Observation->BusReceivedAt100ns ||
        !Observation->BusCompletedAt100ns || !Observation->BusStatus ||
        (*Observation->BusStatus & profile::NTStatusFailureMask))
      return stackError("successful WAIT_WAKE requires actual successful "
                        "provider completion");
  }
  IRPCompletionPlan Plan;
  Plan.StartSlot = *Cursor;
  bool PropagatePending = false;
  for (uint32_t Slot = *Cursor; Slot < Request.StackCount; ++Slot) {
    const uint64_t Stack = IRP + IRPSize + Slot * StackSize;
    auto Control = Memory.readInteger(Stack + StackControlOffset, 1);
    auto PC = Memory.readInteger(Stack + StackCompletionOffset, 8);
    auto Context = Memory.readInteger(Stack + StackCompletionContextOffset, 8);
    if (!Control || !PC || !Context)
      return llvm::joinErrors(
          Control.takeError(),
          llvm::joinErrors(PC.takeError(), Context.takeError()));
    if (*Control & ~CompletionControlMask)
      return stackError("unsupported completion-stack control flags");
    if ((*Control & ~StackPendingReturned) && !*PC)
      return stackError("completion invocation flags require a callback");
    const bool Pending = PropagatePending || (*Control & StackPendingReturned);
    Plan.Steps.push_back({Slot, Pending});
    const bool Success =
        (uint32_t(*Status) & profile::NTStatusFailureMask) == 0;
    const bool Invoke = (Success && (*Control & StackInvokeOnSuccess)) ||
                        (!Success && (*Control & StackInvokeOnError)) ||
                        (*Cancel && (*Control & StackInvokeOnCancel));
    if (Invoke) {
      Plan.PC = *PC;
      Plan.Context = *Context;
      if (Slot + 1 < Request.StackCount) {
        auto Upper =
            Memory.readInteger(Stack + StackSize + StackDeviceOffset, 8);
        if (!Upper)
          return Upper.takeError();
        const auto Allocation = DriverIRPs.find(IRP);
        const bool CallerDevice = Allocation != DriverIRPs.end() &&
                                  Slot == Allocation->second.DispatchSlot &&
                                  *Upper == Allocation->second.CompletionDevice;
        if (!Devices.count(*Upper) ||
            (!CallerDevice &&
             std::find(Request.DeviceRoute.begin(), Request.DeviceRoute.end(),
                       *Upper) == Request.DeviceRoute.end()))
          return stackError("completion lost the upper device identity");
        Plan.Device = *Upper;
      }
      Plan.Arguments = {Plan.Device, IRP, Plan.Context};
      return Plan;
    }
    PropagatePending = Pending;
  }
  if (auto A = DriverIRPs.find(IRP);
      A != DriverIRPs.end() && !A->second.Builder)
    return stackError("caller IRP completion must return "
                      "STATUS_MORE_PROCESSING_REQUIRED");
  llvm::Expected<bool> Pending =
      Plan.Steps.empty() ? dispatchPending(Request, Request.StackCount - 1)
                         : llvm::Expected<bool>(Plan.Steps.back().Pending);
  if (!Pending)
    return Pending.takeError();
  if (auto E = validateCompletionPending(Request, *Pending))
    return E;
  llvm::Expected<uint64_t> Information =
      StatusOverride ? llvm::Expected<uint64_t>(0)
                     : Memory.readInteger(IRP + IRPInformationOffset, 8);
  if (!Information)
    return Information.takeError();
  if (auto E = validateRequestCompletion(IRP, uint32_t(*Status), *Information))
    return E;
  if (Request.PnpTicket)
    if (auto E = validatePnpRequestCompletion(Request, uint32_t(*Status),
                                              StatusOverride.has_value()))
      return E;
  if (Request.PnpTicket && !StatusOverride &&
      !(uint32_t(*Status) & profile::NTStatusFailureMask)) {
    const auto &Observation = Result.Requests[Request.ResultIndex].Pnp;
    if (!Observation || !Observation->BusReceivedAt100ns ||
        !Observation->BusCompletedAt100ns)
      return stackError(
          "successful PnP completion requires actual provider completion");
  }
  if (Request.LifecycleIo)
    if (auto E = Lifecycle.validateIoCompletion(Request.PnpDevice, IRP))
      return E;
  if (Request.PowerTicket) {
    if (auto E = validatePowerRequestCompletion(Request, uint32_t(*Status)))
      return E;
    if (!StatusOverride &&
        !(uint32_t(*Status) & profile::NTStatusFailureMask)) {
      const auto &Observation = Result.Requests[Request.ResultIndex].Power;
      if (!Observation || !Observation->BusReceivedAt100ns ||
          !Observation->BusCompletedAt100ns)
        return stackError(
            "successful power completion requires actual provider completion");
    }
  }
  if (Request.ChildPower && Request.ChildPower->Callback) {
    const auto &Child = *Request.ChildPower;
    if (Child.CallbackStarted || Child.CallbackReturned || !Child.StatusBlock ||
        !Request.PowerOperation)
      return stackError("child power completion lost its callback identity");
    Plan.PowerCompletion = true;
    Plan.PC = Child.Callback;
    Plan.Arguments = {
        Child.RequestDevice, uint8_t(Request.PowerOperation->Minor),
        Request.PowerOperation->State, Child.Context, Child.StatusBlock};
  }
  return Plan;
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::advanceIRPCompletion(uint64_t Token) {
  auto Call = IRPCalls.find(Token);
  if (Call == IRPCalls.end() || Call->second.Kind != IRPCallKind::Completion)
    return stackError("unknown completion continuation");
  const uint64_t IRP = Call->second.IRP;
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return stackError("completion continued after packet retirement");
  auto Plan = planIRPCompletion(IRP);
  if (!Plan)
    return Plan.takeError();
  if (Plan->StartSlot != Call->second.Slot)
    return stackError("completion callback changed its continuation cursor");
  for (const auto &Step : Plan->Steps) {
    const uint64_t Stack = IRP + IRPSize + Step.Slot * StackSize;
    Request->UnwoundPending[Step.Slot] = Step.Pending;
    // Consume the lower slot before entering its upper completion callback.
    const std::array<uint8_t, StackSize> Cleared{};
    if (auto E = Memory.write(Stack, Cleared))
      return E;
    const uint32_t Next = Step.Slot + 1;
    const uint64_t NextStack = Stack + StackSize;
    if (auto E = Memory.writeInteger(IRP + IRPPendingOffset, Step.Pending, 1))
      return E;
    if (auto E = Memory.writeInteger(IRP + IRPLocationOffset, Next + 1, 1))
      return E;
    if (auto E = Memory.writeInteger(IRP + IRPStackPointerOffset, NextStack, 8))
      return E;
    Call->second.Slot = Next;
    const bool Invokes = Plan->PC && !Plan->PowerCompletion &&
                         Next == Plan->Steps.back().Slot + 1;
    if (!Invokes && Step.Pending && Next < Request->StackCount) {
      auto Control = Memory.readInteger(NextStack + StackControlOffset, 1);
      if (!Control)
        return Control.takeError();
      if (auto E = Memory.writeInteger(NextStack + StackControlOffset,
                                       *Control | StackPendingReturned, 1))
        return E;
    }
  }
  if (Plan->PC) {
    if (Plan->PowerCompletion) {
      // All IoCompletion callbacks have returned. Retire the original packet
      // before exposing the independent terminal-callback status snapshot.
      if (auto E = retireCompletedRequest(IRP, 0))
        return E;
      Request->ChildPower->CallbackStarted = true;
      Call->second.Kind = IRPCallKind::PowerCompletion;
    }
    Call->second.AwaitingCallback = true;
    PendingWdmCall = KernelGuestCall{
        {GuestCallOwner::WDM, Token}, Plan->PC, std::move(Plan->Arguments)};
    return std::optional<uint64_t>{};
  }
  const uint64_t ReturnValue = Call->second.ReturnValue;
  if (auto E = retireCompletedRequest(IRP, 0))
    return E;
  IRPCalls.erase(Call);
  if (auto E = tryFinalizeFrameworkUsbIdle(IRP))
    return E;
  if (auto E = tryFinalizePowerRequest(IRP))
    return E;
  if (auto E = tryFinalizeFileClose(IRP))
    return E;
  if (auto E = tryFinalizeDriverIRP(IRP))
    return E;
  return std::optional<uint64_t>{ReturnValue};
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishWdmGuestCall(uint64_t Token, uint64_t ResultValue) {
  const auto Found = IRPCalls.find(Token);
  const auto Parent = Found != IRPCalls.end() ? Found->second.ProviderReceiptIRP
                                              : std::optional<uint64_t>{};
  auto Returned = finishWdmGuestCallBody(Token, ResultValue);
  if (!Returned || !*Returned || !Parent)
    return Returned;
  auto Continued = continueProviderReceipt(*Parent);
  if (!Continued)
    return Continued.takeError();
  if (!*Continued && PendingWdmCall && Scheduler.active()) {
    auto Scheduled = ScheduledModelContinuations.find(Scheduler.active()->ID);
    if (Scheduled != ScheduledModelContinuations.end() &&
        Scheduled->second.Owner == GuestCallOwner::WDM &&
        Scheduled->second.ID == Token)
      Scheduled->second = PendingWdmCall->Token;
  }
  return Continued;
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishWdmGuestCallBody(uint64_t Token, uint64_t ResultValue) {
  auto Call = IRPCalls.find(Token);
  if (Call == IRPCalls.end() || !Call->second.AwaitingCallback)
    return stackError("unknown or inactive guest continuation");
  if (Call->second.Kind == IRPCallKind::FileClose)
    return finishFileCloseDispatch(Token, ResultValue);
  if (Call->second.Kind == IRPCallKind::FileHandleClose)
    return finishFileHandleDispatch(Token, ResultValue);
  if (Call->second.Kind == IRPCallKind::FileRequest)
    return finishFileRequestDispatch(Token, ResultValue);
  if (Call->second.Kind == IRPCallKind::FileOperation)
    return finishFileOperationDispatch(Token, ResultValue);
  if (Call->second.Kind == IRPCallKind::PowerCompletion)
    return finishPowerCompletion(Token);
  if (Call->second.Kind == IRPCallKind::Cancel) {
    if (!CancelLock.Callback || CancelLock.Held ||
        CancelLock.IRP != Call->second.IRP ||
        CurrentIRQL != CancelLock.CallbackIRQL)
      return stackError("IoCancelIrp callback did not release its cancel lock");
    const uint64_t IRP = Call->second.IRP;
    CancelLock = {};
    IRPCalls.erase(Call);
    if (auto E = tryFinalizeDriverIRP(IRP))
      return E;
    if (auto E = tryFinalizeFileClose(IRP))
      return E;
    if (auto E = tryFinalizePowerRequest(IRP))
      return E;
    return std::optional<uint64_t>{1};
  }
  if (Call->second.Kind == IRPCallKind::PowerDispatch) {
    const uint64_t IRP = Call->second.IRP;
    if (auto E = recordDispatchReturn(IRP, uint32_t(ResultValue)))
      return E;
    IRPCalls.erase(Call);
    if (auto E = tryFinalizePowerRequest(IRP))
      return E;
    return std::optional<uint64_t>{StatusPending};
  }
  if (Call->second.Kind == IRPCallKind::Dispatch) {
    const uint64_t IRP = Call->second.IRP;
    const auto *Request = requestForIRP(IRP);
    if (!Request)
      return stackError("dispatch return lost its retained request");
    const uint32_t Status = uint32_t(ResultValue);
    if (auto Allocation = DriverIRPs.find(IRP);
        Allocation != DriverIRPs.end() &&
        Allocation->second.InitialDispatchToken == Token)
      if (auto E = recordDriverIRPDispatchReturn(IRP, Status))
        return E;
    // A forwarding middle driver can return pending before its completion
    // routine runs and propagates that bit. IoCallDriver returns the driver's
    // status now; final unwinding owns propagation and retirement checks.
    IRPCalls.erase(Call);
    if (auto E = tryFinalizePowerRequest(IRP))
      return E;
    if (auto E = tryFinalizeDriverIRP(IRP))
      return E;
    if (auto E = tryFinalizeFileClose(IRP))
      return E;
    return std::optional<uint64_t>{Status};
  }
  const uint64_t IRP = Call->second.IRP;
  auto Allocation = DriverIRPs.find(IRP);
  if (Allocation != DriverIRPs.end() && Allocation->second.StorageReleased &&
      Allocation->second.FreeCompletionToken == Token &&
      uint32_t(ResultValue) != StatusMoreProcessingRequired)
    return stackError("IoFreeIrp in completion requires "
                      "STATUS_MORE_PROCESSING_REQUIRED");
  if (uint32_t(ResultValue) == StatusMoreProcessingRequired &&
      Allocation != DriverIRPs.end() && !Allocation->second.StorageReleased &&
      Call->second.Slot == Allocation->second.DispatchSlot + 1) {
    if (!Allocation->second.Builder)
      if (auto E = captureDriverIRPCompletion(IRP))
        return E;
    Allocation->second.CompletionHeld = true;
  }
  Call->second.AwaitingCallback = false;
  if (uint32_t(ResultValue) == StatusMoreProcessingRequired) {
    // A legal nested IoCompleteRequest may already have retired the packet.
    // This return stops only this invocation's unwind; never read that packet.
    const uint64_t IRP = Call->second.IRP;
    const uint64_t ReturnValue = Call->second.ReturnValue;
    IRPCalls.erase(Call);
    if (auto E = tryFinalizePowerRequest(IRP))
      return E;
    if (auto E = tryFinalizeDriverIRP(IRP))
      return E;
    if (auto E = tryFinalizeFileClose(IRP))
      return E;
    return std::optional<uint64_t>{ReturnValue};
  }
  return advanceIRPCompletion(Token);
}
} // namespace neverd::emulation
