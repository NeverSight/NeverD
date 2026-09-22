//===- KernelModelFramework.cpp - WDF module dispatch ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bridge authoritative PE export identities to the independent WDF model.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

llvm::Error frameworkDeviceError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

void KernelModel::configureFrameworkDeviceHost() {
  KernelFramework::DeviceHost Host;
  Host.Create =
      [this](llvm::StringRef Name,
             bool Direct) -> llvm::Expected<KernelFramework::DeviceCreation> {
    auto Created = createDeviceObject(Name, 0, windows::UnknownDeviceType,
                                      windows::SecureOpen, false);
    if (!Created)
      return Created.takeError();
    if (Created->Status != windows::StatusSuccess)
      return KernelFramework::DeviceCreation{Created->Status, 0};
    const uint64_t Device = Created->Address;
    auto Flags = Memory.readInteger(Device + windows::DeviceFlagsOffset, 4);
    if (!Flags)
      return llvm::joinErrors(Flags.takeError(), deleteDevice(Device));
    const uint32_t TransferFlags =
        Direct ? windows::DeviceDirectIO : windows::DeviceBufferedIO;
    if (auto E = Memory.writeInteger(
            Device + windows::DeviceFlagsOffset,
            (*Flags & ~(windows::DeviceBufferedIO | windows::DeviceDirectIO)) |
                TransferFlags,
            4))
      return llvm::joinErrors(std::move(E), deleteDevice(Device));
    FrameworkDevices.emplace(Device, std::vector<std::string>{});
    return KernelFramework::DeviceCreation{windows::StatusSuccess, Device};
  };
  Host.FinishInitializing = [this](uint64_t Device) -> llvm::Error {
    if (!FrameworkDevices.count(Device) || !Devices.count(Device) ||
        Devices.at(Device).DeletePending)
      return frameworkDeviceError(
          "framework initialization requires its own live WDM device");
    auto Flags = Memory.readInteger(Device + windows::DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    return Memory.writeInteger(Device + windows::DeviceFlagsOffset,
                               *Flags & ~windows::DeviceInitializing, 4);
  };
  Host.Link = [this](uint64_t Device,
                     llvm::StringRef Name) -> llvm::Expected<uint32_t> {
    auto Owner = FrameworkDevices.find(Device);
    auto Object = Devices.find(Device);
    if (Owner == FrameworkDevices.end() || Object == Devices.end() ||
        Object->second.DeletePending)
      return frameworkDeviceError(
          "framework symbolic link requires its own live WDM device");
    auto Key = linkKey(Name);
    if (!Key)
      return Key.takeError();
    auto Status = createSymbolicLink(Name, Object->second.Name);
    if (!Status)
      return Status.takeError();
    if (*Status == windows::StatusSuccess)
      Owner->second.push_back(std::move(*Key));
    return *Status;
  };
  Host.Delete = [this](uint64_t Device) -> llvm::Error {
    auto Owner = FrameworkDevices.find(Device);
    auto Object = Devices.find(Device);
    if (Owner == FrameworkDevices.end() || Object == Devices.end() ||
        Object->second.DeletePending)
      return frameworkDeviceError(
          "framework deletion requires its own live WDM device");
    if (std::any_of(Files.begin(), Files.end(), [&](const auto &File) {
          return File.second.Address && File.second.Device == Device;
        }))
      return frameworkDeviceError(
          "framework device deletion with live files is outside this profile");
    if (std::any_of(WorkItems.begin(), WorkItems.end(),
                    [&](const auto &Item) { return Item.second == Device; }))
      return frameworkDeviceError(
          "framework device deletion with live work items is outside this "
          "profile");
    auto References = WorkReferences.find(Device);
    if (Scheduler.hasOutstanding(Device) ||
        (References != WorkReferences.end() && References->second))
      return frameworkDeviceError(
          "asynchronous framework device deletion is outside this profile");
    // Completed packets may already be inaccessible while their dispatch
    // invocation has not returned. Model-owned identity survives until the
    // request record is finalized; never recover it from retired guest fields.
    if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          return Entry.second.Device == Device;
        }))
      return frameworkDeviceError(
          "framework device deletion with an active IRP is outside this "
          "profile");
    for (const auto &Key : Owner->second) {
      auto Link = SymbolicLinks.find(Key);
      if (Link == SymbolicLinks.end() || Link->second != Object->second.Name)
        return frameworkDeviceError(
            "framework device symbolic-link ownership changed before deletion");
    }
    // Validate the authoritative list before changing dispatcher storage or
    // marking deletion. No guest callback can interleave these host operations.
    if (auto E = snapshot())
      return E;
    if (auto E = prepareReleaseRange(Device, Object->second.Size))
      return E;
    if (auto E = deleteDevice(Device))
      return E;
    for (const auto &Key : Owner->second) {
      auto Status = deleteSymbolicLink(Key);
      if (!Status)
        return Status.takeError();
      if (*Status != windows::StatusSuccess)
        return frameworkDeviceError(
            "framework symbolic link changed after deletion preflight");
    }
    FrameworkDevices.erase(Owner);
    return llvm::Error::success();
  };
  Framework->setDeviceHost(std::move(Host));
}

std::optional<unsigned>
KernelModel::argumentCount(const KernelExportRegistry::Export &Export) {
  if (Export.Kind == KernelExportRegistry::ExportKind::ModuleExport &&
      Export.Module == KernelProvider)
    return argumentCount(Export.Name);
  if (Export.Kind == KernelExportRegistry::ExportKind::DMAFunction)
    return dmaArgumentCount(Export.Name);
  return KernelFramework::argumentCount(Export);
}

llvm::Expected<uint64_t> KernelModel::call(
    const KernelExportRegistry::Export &Export,
    llvm::ArrayRef<uint64_t> Arguments,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument) {
  if (Export.Kind == KernelExportRegistry::ExportKind::ModuleExport &&
      Export.Module == KernelProvider)
    return call(Export.Name, Arguments, ReadArgument);
  if (Export.Kind == KernelExportRegistry::ExportKind::DMAFunction)
    return callDMAExport(Export, Arguments);
  if (!Framework)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "framework model is not initialized");
  if (PendingWdmCall)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "cannot replace a pending WDM guest callback");
  return Framework->call(Export, Arguments, CurrentIRQL);
}

std::optional<KernelGuestCall> KernelModel::takeGuestCall() {
  if (PendingDMACall)
    return std::exchange(PendingDMACall, std::nullopt);
  if (PendingInterruptCall)
    return std::exchange(PendingInterruptCall, std::nullopt);
  if (auto Call = takeWdmGuestCall())
    return Call;
  if (Framework)
    if (auto Call = Framework->takeGuestCall())
      return KernelGuestCall{{GuestCallOwner::Framework, Call->Token}, Call->PC,
                             std::move(Call->Arguments)};
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishGuestCall(GuestCallToken Token, uint64_t Result) {
  if (!Token.ID)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "guest callback has no continuation token");
  switch (Token.Owner) {
  case GuestCallOwner::Framework:
    if (!Framework)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "framework callback has no owning model");
    return Framework->finishGuestCall(Token.ID, Result);
  case GuestCallOwner::WDM:
    return finishWdmGuestCall(Token.ID, Result);
  case GuestCallOwner::Interrupt:
    return finishInterruptCall(Token.ID, Result);
  case GuestCallOwner::DMA:
    return finishDMACall(Token.ID);
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "guest callback has an invalid owner");
}
} // namespace neverd::emulation
