//===- KernelModelResources.cpp - PnP resource packet ownership
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Deliver the configured raw/translated lists and advance hardware facts at
/// actual lower-driver completion, independently of upper completion state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {

llvm::Error KernelModel::initializePnpResources(ActiveRequest &Request) {
  if (Request.PnpOperation->Minor == DevicePnpRequest::Start &&
      MMIO.hasResources(Request.PnpDevice)) {
    auto Raw = MMIO.resourceList(Request.PnpDevice, false);
    auto Translated = MMIO.resourceList(Request.PnpDevice, true);
    if (!Raw || !Translated)
      return llvm::joinErrors(Raw.takeError(), Translated.takeError());
    auto RawAddress = allocate(Raw->size());
    if (!RawAddress)
      return RawAddress.takeError();
    auto TranslatedAddress = allocate(Translated->size());
    if (!TranslatedAddress)
      return TranslatedAddress.takeError();
    Request.RawResources = *RawAddress;
    Request.TranslatedResources = *TranslatedAddress;
    Request.ResourceListSize = Raw->size();
    if (auto E = Memory.write(*RawAddress, *Raw))
      return E;
    if (auto E = Memory.write(*TranslatedAddress, *Translated))
      return E;
  }
  if (auto E = Memory.writeInteger(Request.Stack +
                                       windows::StackStartResourcesOffset,
                                   Request.RawResources, profile::PointerSize))
    return E;
  return Memory.writeInteger(Request.Stack +
                                 windows::StackStartTranslatedResourcesOffset,
                             Request.TranslatedResources, profile::PointerSize);
}

llvm::Error KernelModel::validatePnpRequestCompletion(
    const ActiveRequest &Request, uint32_t Status, bool ProviderProbe) const {
  if (auto E = Lifecycle.validatePnpCompletion(*Request.PnpTicket, Status))
    return E;
  // A provider probe runs before the bus activates resources. The real final
  // completion repeats this validation after actual provider completion.
  if (ProviderProbe && Request.PnpOperation->Minor == DevicePnpRequest::Start)
    return llvm::Error::success();
  return MMIO.validateCompletion(Request.PnpDevice, Request.PnpOperation->Minor,
                                 Status);
}

llvm::Error KernelModel::publishProviderHardware(ActiveRequest &Request,
                                                 uint32_t Status) {
  if (Request.PnpOperation &&
      Request.PnpOperation->Minor == DevicePnpRequest::Start)
    return MMIO.completeLowerStart(Request.PnpDevice, Status);
  if (Request.PowerOperation &&
      Request.PowerOperation->Type == DriverPowerType::Device &&
      Request.PowerOperation->Minor == DevicePowerRequest::Set &&
      !(Status & profile::NTStatusFailureMask))
    MMIO.setPhysicalPower(
        Request.PnpDevice,
        static_cast<DevicePowerState>(Request.PowerOperation->State));
  return llvm::Error::success();
}

} // namespace neverd::emulation
