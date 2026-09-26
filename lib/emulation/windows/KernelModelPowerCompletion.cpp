//===- KernelModelPowerCompletion.cpp - Requested power IRP lifetime -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Real PoRequestPowerIrp dispatch and its terminal void callback retain
/// independent request identities and never borrow a scenario request result.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error powerError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "requested power IRP: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::requestPowerIrp(llvm::ArrayRef<uint64_t> Arguments) {
  if (Arguments.size() != 6)
    return powerError("PoRequestPowerIrp requires six arguments");
  // The real DDI permits DISPATCH_LEVEL. This pageable profile has no queued
  // power-dispatch adapter, so never lower an executing DPC's IRQL implicitly.
  if (CurrentIRQL != scheduler::PassiveLevel)
    return powerError("this profile requires PASSIVE_LEVEL PoRequestPowerIrp");
  const auto Minor = static_cast<DevicePowerRequest>(uint8_t(Arguments[1]));
  if (Minor != DevicePowerRequest::Set && Minor != DevicePowerRequest::Query) {
    if (!uint8_t(Arguments[1]))
      return powerError("WAIT_WAKE is outside the requested power profile");
    return StatusInvalidParameter2;
  }
  if (Arguments[5])
    return powerError("Query/Set power requires a null output IRP pointer");
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return powerError("cannot replace a pending guest callback");
  if (NextIRPCall == UINT64_MAX)
    return powerError("continuation identity exhausted");
  const uint64_t Device = Arguments[0];
  auto PDO = pnpDeviceForRoute(Device);
  if (!PDO)
    return PDO.takeError();
  auto *Provider = pnpDeviceForPDO(*PDO);
  if (!*PDO || !Provider || !Devices.count(Device) ||
      Devices.at(Device).DeletePending)
    return powerError("request requires a live configured PDO or FDO");
  const uint32_t Index = Provider->RequestedPowerIndex;
  if (Index >= Provider->RequestedDevicePower.size())
    return powerError("requested_device_power response FIFO is exhausted");
  const auto &Operation = Provider->RequestedDevicePower[Index];
  if (Operation.Type != DriverPowerType::Device || Operation.Minor != Minor ||
      Operation.State != uint32_t(Arguments[2]))
    return powerError("PoRequestPowerIrp does not match the next explicit "
                      "requested_device_power response");
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = Result.PnpDevices[Provider->ResultIndex].ID;
  Input.Power = Operation;
  RequestedPower Child;
  Child.RequestDevice = Device;
  Child.Callback = Arguments[3];
  Child.Context = Arguments[4];
  Child.ResponseIndex = Index;
  auto Invocation = preparePowerRequest(Input, Result.Requests.size(), Child);
  if (!Invocation)
    return Invocation.takeError();
  // Preparation publishes a distinct packet/result only after validation.
  // The immutable provider record remains live through the retained route.
  ++Provider->RequestedPowerIndex;
  const uint64_t IRP = Invocation->IRP;
  if (Invocation->PC) {
    const uint64_t Token = NextIRPCall++;
    IRPCalls.emplace(Token,
                     IRPCall{IRPCallKind::PowerDispatch, IRP,
                             uint32_t(requestForIRP(IRP)->StackCount - 1), true,
                             StatusPending});
    PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                     Invocation->PC,
                                     {Invocation->Argument0, IRP}};
    return StatusPending;
  }
  auto Status = callProviderDriver(Invocation->Argument0, IRP);
  if (!Status)
    return Status.takeError();
  if (auto E = recordDispatchReturn(IRP, uint32_t(*Status)))
    return std::move(E);
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = StatusPending;
  if (auto E = tryFinalizePowerRequest(IRP))
    return std::move(E);
  return StatusPending;
}

llvm::Error KernelModel::tryFinalizePowerRequest(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->ChildPower || !Request->Completed ||
      !Request->DispatchReturned)
    return llvm::Error::success();
  const auto &Child = *Request->ChildPower;
  if (Child.Callback && !Child.CallbackReturned)
    return llvm::Error::success();
  if (std::any_of(IRPCalls.begin(), IRPCalls.end(),
                  [&](const auto &Entry) { return Entry.second.IRP == IRP; }))
    return llvm::Error::success();
  return finalizeRequest(IRP);
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishPowerCompletion(uint64_t Token) {
  auto Call = IRPCalls.find(Token);
  if (Call == IRPCalls.end() ||
      Call->second.Kind != IRPCallKind::PowerCompletion ||
      !Call->second.AwaitingCallback)
    return powerError("unknown or inactive PowerCompletion continuation");
  const uint64_t IRP = Call->second.IRP;
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->Completed || !Request->ChildPower)
    return powerError("PowerCompletion lost its completed child identity");
  auto &Child = *Request->ChildPower;
  if (!Child.CallbackStarted || Child.CallbackReturned || !Child.StatusBlock)
    return powerError("PowerCompletion lost its status snapshot");
  if (auto E = prepareReleaseRange(Child.StatusBlock, PowerStatusBlockSize))
    return E;
  FreedRanges.emplace(Child.StatusBlock, PowerStatusBlockSize);
  Child.CallbackReturned = true;
  const uint64_t ReturnValue = Call->second.ReturnValue;
  IRPCalls.erase(Call);
  if (auto E = tryFinalizePowerRequest(IRP))
    return E;
  // REQUEST_POWER_COMPLETE is void; its guest RAX is never a completion status.
  return std::optional<uint64_t>{ReturnValue};
}
} // namespace neverd::emulation
