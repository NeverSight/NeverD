//===- KernelModelIRPStack.cpp - WDM dispatch and completion continuations
//--===//
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

llvm::Expected<uint64_t> KernelModel::callDriver(uint64_t Device,
                                                 uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return stackError("IoCallDriver requires a live owned IRP");
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return stackError("cannot replace a pending guest callback");
  if (Framework && Framework->ownsRequestIRP(IRP))
    return stackError("framework-owned requests cannot use WDM forwarding");
  if (!Devices.count(Device) ||
      std::find(Request->DeviceRoute.begin(), Request->DeviceRoute.end(),
                Device) == Request->DeviceRoute.end())
    return stackError("target is outside the request's retained device route");
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
  auto PC = Memory.readInteger(
      Devices.at(Device).OwnerDriver + DriverDispatchOffset + *Major * 8, 8);
  if (!PC)
    return PC.takeError();
  if (!*PC) {
    const auto First = DispatchBytesWritten.begin() + *Major * 8;
    if (std::any_of(First, First + 8, [](bool Written) { return Written; }))
      return stackError(
          "driver explicitly registered a null dispatch callback");
  }
  if (NextIRPCall == UINT64_MAX)
    return stackError("continuation identity exhausted");
  if (auto E = Memory.writeInteger(IRP + IRPLocationOffset, Slot + 1, 1))
    return std::move(E);
  if (auto E = Memory.writeInteger(IRP + IRPStackPointerOffset, Stack, 8))
    return std::move(E);
  if (auto E = Memory.writeInteger(Stack + StackDeviceOffset, Device, 8))
    return std::move(E);
  Request->Forwarded = true;
  Request->UnwoundPending[Slot].reset();
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
  if (PriorityBoost)
    return stackError("modeled completion supports IO_NO_INCREMENT only");
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return stackError("cannot replace a pending guest callback");
  for (unsigned I = 0; I < Request->IOStatusWritten.size(); ++I)
    if ((I < 4 || I >= 8) && !Request->IOStatusWritten[I])
      return stackError(
          "completion requires initialized IoStatus.Status and Information");
  auto Status = Memory.readInteger(IRP + IRPStatusOffset, 4);
  auto Information = Memory.readInteger(IRP + IRPInformationOffset, 8);
  if (!Status || !Information)
    return llvm::joinErrors(Status.takeError(), Information.takeError());
  if (uint32_t(*Status) == StatusPending)
    return stackError("IoCompleteRequest cannot complete with STATUS_PENDING");
  auto Cursor = requestStackCursor(IRP);
  if (!Cursor)
    return Cursor.takeError();
  if (NextIRPCall == UINT64_MAX)
    return stackError("continuation identity exhausted");
  const uint64_t Token = NextIRPCall++;
  IRPCalls.emplace(Token, IRPCall{IRPCallKind::Completion, IRP, *Cursor});
  auto Result = advanceIRPCompletion(Token);
  if (!Result)
    return Result.takeError();
  return llvm::Error::success();
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
  while (true) {
    auto Cursor = requestStackCursor(IRP);
    if (!Cursor)
      return Cursor.takeError();
    if (*Cursor != Call->second.Slot)
      return stackError("completion callback changed its continuation cursor");
    if (*Cursor == Request->StackCount) {
      const uint64_t ReturnValue = Call->second.ReturnValue;
      if (auto E = retireCompletedRequest(IRP, 0))
        return std::move(E);
      IRPCalls.erase(Call);
      return std::optional<uint64_t>{ReturnValue};
    }
    const uint64_t Stack = IRP + IRPSize + *Cursor * StackSize;
    auto Control = Memory.readInteger(Stack + StackControlOffset, 1);
    auto PC = Memory.readInteger(Stack + StackCompletionOffset, 8);
    auto Context = Memory.readInteger(Stack + StackCompletionContextOffset, 8);
    auto Status = Memory.readInteger(IRP + IRPStatusOffset, 4);
    auto Cancel = Memory.readInteger(IRP + IRPCancelOffset, 1);
    if (!Control || !PC || !Context || !Status || !Cancel)
      return llvm::joinErrors(
          llvm::joinErrors(Control.takeError(), PC.takeError()),
          llvm::joinErrors(
              Context.takeError(),
              llvm::joinErrors(Status.takeError(), Cancel.takeError())));
    if (*Control & ~CompletionControlMask)
      return stackError("unsupported completion-stack control flags");
    const bool Pending = (*Control & StackPendingReturned) != 0;
    const bool Success = (uint32_t(*Status) & 0x80000000U) == 0;
    const bool Invoke = (Success && (*Control & StackInvokeOnSuccess)) ||
                        (!Success && (*Control & StackInvokeOnError)) ||
                        (*Cancel && (*Control & StackInvokeOnCancel));
    if ((*Control & ~StackPendingReturned) && !*PC)
      return stackError("completion invocation flags require a callback");
    Request->UnwoundPending[*Cursor] = Pending;
    // Windows clears each consumed lower location before invoking the upper
    // completion. Preserve only callback inputs and the historical pending bit.
    // https://learn.microsoft.com/windows-hardware/drivers/kernel/implementing-an-iocompletion-routine
    const std::array<uint8_t, StackSize> Cleared{};
    if (auto E = Memory.write(Stack, Cleared))
      return std::move(E);
    const uint32_t Next = *Cursor + 1;
    const uint64_t NextStack = IRP + IRPSize + Next * StackSize;
    if (auto E = Memory.writeInteger(IRP + IRPPendingOffset, Pending, 1))
      return std::move(E);
    if (auto E = Memory.writeInteger(IRP + IRPLocationOffset, Next + 1, 1))
      return std::move(E);
    if (auto E = Memory.writeInteger(IRP + IRPStackPointerOffset, NextStack, 8))
      return std::move(E);
    Call->second.Slot = Next;
    if (Invoke) {
      uint64_t Device = 0;
      if (Next < Request->StackCount) {
        auto Upper = Memory.readInteger(NextStack + StackDeviceOffset, 8);
        if (!Upper)
          return Upper.takeError();
        if (!Devices.count(*Upper) ||
            std::find(Request->DeviceRoute.begin(), Request->DeviceRoute.end(),
                      *Upper) == Request->DeviceRoute.end())
          return stackError("completion lost the upper device identity");
        Device = *Upper;
      }
      Call->second.AwaitingCallback = true;
      PendingWdmCall = KernelGuestCall{
          {GuestCallOwner::WDM, Token}, *PC, {Device, IRP, *Context}};
      return std::optional<uint64_t>{};
    }
    // Without a completion routine, the I/O manager propagates pending to
    // the next slot. An invoked routine instead owns this decision itself.
    if (Pending && Next < Request->StackCount) {
      auto UpperControl = Memory.readInteger(NextStack + StackControlOffset, 1);
      if (!UpperControl)
        return UpperControl.takeError();
      if (auto E = Memory.writeInteger(NextStack + StackControlOffset,
                                       *UpperControl | StackPendingReturned, 1))
        return std::move(E);
    }
  }
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishWdmGuestCall(uint64_t Token, uint64_t ResultValue) {
  auto Call = IRPCalls.find(Token);
  if (Call == IRPCalls.end() || !Call->second.AwaitingCallback)
    return stackError("unknown or inactive guest continuation");
  if (Call->second.Kind == IRPCallKind::Dispatch) {
    const auto *Request = requestForIRP(Call->second.IRP);
    if (!Request)
      return stackError("dispatch return lost its retained request");
    const uint32_t Status = uint32_t(ResultValue);
    // A forwarding middle driver can return pending before its completion
    // routine runs and propagates that bit. IoCallDriver returns the driver's
    // status now; final unwinding owns propagation and retirement checks.
    IRPCalls.erase(Call);
    return std::optional<uint64_t>{Status};
  }
  Call->second.AwaitingCallback = false;
  if (uint32_t(ResultValue) == StatusMoreProcessingRequired) {
    // A legal nested IoCompleteRequest may already have retired the packet.
    // This return stops only this invocation's unwind; never read that packet.
    const uint64_t ReturnValue = Call->second.ReturnValue;
    IRPCalls.erase(Call);
    return std::optional<uint64_t>{ReturnValue};
  }
  return advanceIRPCompletion(Token);
}
} // namespace neverd::emulation
