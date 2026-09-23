//===- KernelFrameworkControl.cpp - KMDF control-device initialization
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own WDF initializers and handles while the host bridge owns the underlying
/// WDM device, namespace and memory. This profile requires a named control
/// device and an explicit universal-access DACL; it never guesses a caller's
/// Windows token or claims to enforce an unmodeled security descriptor.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

namespace neverd::emulation {
namespace {
using namespace framework;
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

llvm::Error controlError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF control device: " + Message);
}
} // namespace

llvm::Expected<std::string>
KernelFramework::readControlString(uint64_t Address) {
  auto Length = read(Address, 2);
  auto Maximum = read(Address + 2, 2);
  auto Buffer = read(Address + 8);
  if (!Length || !Maximum || !Buffer)
    return llvm::joinErrors(
        Length.takeError(),
        llvm::joinErrors(Maximum.takeError(), Buffer.takeError()));
  if (*Length % 2 || *Length > *Maximum || *Length > MaxRegistryPathBytes)
    return controlError("invalid counted Unicode string");
  if (!*Length)
    return std::string{};
  if (auto E = ValidateAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (auto E = Memory.read(*Buffer, Bytes))
    return E;
  std::string Text;
  for (size_t I = 0; I < Bytes.size(); I += 2) {
    if (Bytes[I + 1] || Bytes[I] < ' ' || Bytes[I] > '~')
      return controlError(
          "only printable ASCII namespace and SDDL text is modeled");
    Text.push_back(char(Bytes[I]));
  }
  return Text;
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callControl(llvm::StringRef Name, Binding &B,
                             llvm::ArrayRef<uint64_t> A) {
  using Result = std::optional<uint64_t>;
  if (Name == api::WdfCmResourceListGetCount ||
      Name == api::WdfCmResourceListGetDescriptor) {
    for (const auto &[Handle, Device] : Devices) {
      const auto &Object = Objects.at(Handle);
      if (Object.Binding != B.Globals || Object.Deleting ||
          !Device.ResourcesActive)
        continue;
      if (A[1] == Device.RawResourceList ||
          A[1] == Device.TranslatedResourceList)
        return Result{0};
    }
    return controlError("resource-list query requires a live assigned list");
  }
  if (Name == api::WdfControlDeviceInitAllocate) {
    auto Driver = Objects.find(A[1]);
    if (Driver == Objects.end() || Driver->second.Kind != ObjectKind::Driver ||
        Driver->second.Binding != B.Globals || Driver->second.Deleting)
      return controlError("initializer requires the live bound driver");
    auto SDDL = readControlString(A[2]);
    if (!SDDL)
      return SDDL.takeError();
    if (*SDDL != FrameworkWorldFullAccess)
      return controlError("this profile supports only D:P(A;;GA;;;WD); other "
                          "DACLs require a caller security-token model");
    auto Address = allocate(HandleSize, false, true);
    if (!Address)
      return Address.takeError();
    DeviceInits.emplace(*Address,
                        DeviceInit{B.Globals, DeviceInitKind::Control});
    return Result{*Address};
  }
  if (Name == api::WdfDeviceInitFree || Name == api::WdfDeviceInitAssignName ||
      Name == api::WdfDeviceInitSetIoType ||
      Name == api::WdfDeviceInitSetIoInCallerContextCallback ||
      Name == api::WdfDeviceInitSetPnpPowerEventCallbacks) {
    auto I = DeviceInits.find(A[1]);
    if (I == DeviceInits.end() || I->second.Binding != B.Globals)
      return controlError("invalid, consumed or foreign device initializer");
    if (Name == api::WdfDeviceInitFree) {
      if (I->second.Kind != DeviceInitKind::Control)
        return controlError("framework-owned PnP initializer is freed after "
                            "EvtDriverDeviceAdd returns");
      if (auto E = retire(A[1]))
        return E;
      DeviceInits.erase(I);
    } else if (Name == api::WdfDeviceInitAssignName) {
      if (!A[2]) {
        I->second.Name.clear();
      } else {
        auto Text = readControlString(A[2]);
        if (!Text)
          return Text.takeError();
        I->second.Name = std::move(*Text);
      }
    } else if (Name == api::WdfDeviceInitSetIoInCallerContextCallback) {
      if (!A[2])
        return controlError("caller-context callback must name guest code");
      I->second.CallerContext = A[2];
    } else if (Name == api::WdfDeviceInitSetPnpPowerEventCallbacks) {
      if (I->second.Kind != DeviceInitKind::Pnp)
        return controlError("PnP power callbacks require an FDO initializer");
      if (auto E = ValidateAccess(A[2], PnpPowerCallbacksSize, false))
        return E;
      auto Size = read(A[2], 4);
      if (!Size)
        return Size.takeError();
      if (*Size != PnpPowerCallbacksSize)
        return controlError("unsupported PnP power callback structure size");
      uint64_t Entry = 0, Exit = 0, Prepare = 0, Release = 0;
      for (unsigned Index = 0; Index < PnpPowerCallbacksCount; ++Index) {
        auto Callback = read(A[2] + PnpPowerCallbacksFirstOffset +
                             Index * sizeof(uint64_t));
        if (!Callback)
          return Callback.takeError();
        if (Index == PnpPowerD0EntryIndex)
          Entry = *Callback;
        else if (Index == PnpPowerD0ExitIndex)
          Exit = *Callback;
        else if (Index == PnpPowerPrepareHardwareIndex)
          Prepare = *Callback;
        else if (Index == PnpPowerReleaseHardwareIndex)
          Release = *Callback;
        else if (*Callback)
          return controlError("unsupported PnP power event callback");
      }
      I->second.D0Entry = Entry;
      I->second.D0Exit = Exit;
      I->second.PrepareHardware = Prepare;
      I->second.ReleaseHardware = Release;
    } else {
      if (A[2] != ControlIoNeither && A[2] != ControlIoBuffered &&
          A[2] != ControlIoDirect)
        return controlError("unsupported READ/WRITE I/O type");
      I->second.IoType = uint32_t(A[2]);
    }
    return Result{0};
  }
  if (Name == api::WdfFdoInitWdmGetPhysicalDevice) {
    auto I = DeviceInits.find(A[1]);
    if (I == DeviceInits.end() || I->second.Binding != B.Globals ||
        I->second.Kind != DeviceInitKind::Pnp)
      return controlError("physical device requires a live FDO initializer");
    return Result{I->second.PDO};
  }
  if (Name == api::WdfWdmDeviceGetWdfDeviceHandle) {
    for (const auto &[Handle, Device] : Devices) {
      auto Object = Objects.find(Handle);
      if (Device.Wdm == A[1] && Object != Objects.end() &&
          Object->second.Binding == B.Globals && !Object->second.Deleting)
        return Result{Handle};
    }
    return Result{0};
  }
  if (Name == api::WdfDeviceCreate) {
    if (auto E = writable(A[3], 8))
      return E;
    if (auto E = Memory.writeInteger(A[3], 0, 8))
      return E;
    if (auto E = writable(A[1], 8))
      return E;
    auto InitAddress = read(A[1]);
    if (!InitAddress)
      return InitAddress.takeError();
    auto I = DeviceInits.find(*InitAddress);
    if (I == DeviceInits.end() || I->second.Binding != B.Globals)
      return controlError("creation requires a live device initializer");
    auto Validation = attributes(A[2], AttributesUse::Device);
    if (!Validation)
      return Validation.takeError();
    if (const auto *Status = std::get_if<uint32_t>(&*Validation))
      return Result{*Status};
    if (I->second.Kind == DeviceInitKind::Control && I->second.Name.empty())
      return controlError(
          "unnamed or autogenerated control devices are not modeled");
    if (!DevicesHost.Create || !DevicesHost.Delete ||
        (I->second.Kind == DeviceInitKind::Pnp && !DevicesHost.CreatePnp))
      return controlError("underlying WDM device host is unavailable");
    auto Wdm = I->second.Kind == DeviceInitKind::Control
                   ? DevicesHost.Create(I->second.Name, I->second.IoType)
                   : DevicesHost.CreatePnp(I->second.PDO, I->second.Name,
                                           I->second.IoType);
    if (!Wdm)
      return Wdm.takeError();
    if (Wdm->Status)
      return Result{Wdm->Status};
    auto Handle =
        createObject(B.Globals, std::get<Attributes>(*Validation), false);
    if (!Handle)
      return llvm::joinErrors(Handle.takeError(),
                              DevicesHost.Delete(Wdm->Address));
    Objects.at(*Handle).Kind = ObjectKind::Device;
    Devices.emplace(*Handle, Device{Wdm->Address, I->second.PDO});
    Devices.at(*Handle).CallerContext = I->second.CallerContext;
    Devices.at(*Handle).D0Entry = I->second.D0Entry;
    Devices.at(*Handle).D0Exit = I->second.D0Exit;
    Devices.at(*Handle).PrepareHardware = I->second.PrepareHardware;
    Devices.at(*Handle).ReleaseHardware = I->second.ReleaseHardware;
    if (I->second.Kind == DeviceInitKind::Pnp)
      PnpDeviceHandles.emplace(I->second.PDO, *Handle);
    if (auto E = Memory.writeInteger(A[3], *Handle, 8))
      return E;
    if (auto E = Memory.writeInteger(A[1], 0, 8))
      return E;
    if (auto E = retire(*InitAddress))
      return E;
    DeviceInits.erase(I);
    return Result{0};
  }
  if (Name == api::WdfDeviceEnqueueRequest) {
    auto D = Devices.find(A[1]);
    auto O = Objects.find(A[1]);
    auto R = Requests.find(A[2]);
    if (D == Devices.end() || O == Objects.end() ||
        O->second.Kind != ObjectKind::Device ||
        O->second.Binding != B.Globals || R == Requests.end() ||
        R->second.Completed || R->second.Completing)
      return controlError("enqueue requires a live device and request");
    auto Caller = CallerRequests.find(R->second.IRP);
    if (Caller == CallerRequests.end() || Caller->second != A[2] ||
        !R->second.InCallerContext || R->second.Enqueued ||
        R->second.Device != A[1] || !D->second.DefaultQueue ||
        !Queues.count(D->second.DefaultQueue) ||
        Queues.at(D->second.DefaultQueue).Device != A[1])
      return controlError(
          "request must be enqueued once from its caller-context callback");
    if (!Queues.at(D->second.DefaultQueue).Accepting)
      return Result{QueueBusy};
    R->second.Enqueued = true;
    R->second.Queue = D->second.DefaultQueue;
    return Result{0};
  }
  if (Name != api::WdfDeviceCreateSymbolicLink &&
      Name != api::WdfControlFinishInitializing &&
      Name != api::WdfDeviceWdmGetDeviceObject &&
      Name != api::WdfDeviceWdmGetAttachedDevice &&
      Name != api::WdfDeviceWdmGetPhysicalDevice &&
      Name != api::WdfDeviceGetDriver)
    return Result{};
  auto O = Objects.find(A[1]);
  auto D = Devices.find(A[1]);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Device ||
      O->second.Binding != B.Globals || D == Devices.end())
    return controlError("operation requires a live control-device handle");
  if (Name == api::WdfDeviceWdmGetDeviceObject)
    return Result{D->second.Wdm};
  if (Name == api::WdfDeviceWdmGetAttachedDevice)
    return Result{D->second.PDO};
  if (Name == api::WdfDeviceWdmGetPhysicalDevice)
    return Result{D->second.PDO};
  if (Name == api::WdfDeviceGetDriver)
    return Result{B.DriverHandle};
  if (O->second.Deleting)
    return controlError("cannot change a deleting control device");
  if (Name == api::WdfControlFinishInitializing) {
    if (D->second.PDO)
      return controlError("PnP devices finish initialization after AddDevice");
    if (!DevicesHost.FinishInitializing)
      return controlError("underlying WDM initialization host is unavailable");
    if (auto E = DevicesHost.FinishInitializing(D->second.Wdm))
      return E;
    D->second.Initialized = true;
    return Result{0};
  }
  auto Link = readControlString(A[2]);
  if (!Link)
    return Link.takeError();
  if (Link->empty() || D->second.HasLink)
    return Result{ControlInvalidDeviceRequest};
  if (!DevicesHost.Link)
    return controlError("underlying WDM link host is unavailable");
  auto Status = DevicesHost.Link(D->second.Wdm, *Link);
  if (!Status)
    return Status.takeError();
  if (!*Status)
    D->second.HasLink = true;
  return Result{*Status};
}
} // namespace neverd::emulation
