//===- KernelModelPnpDevices.cpp - PDO and AddDevice ownership -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Provider-owned PDOs use the same device and attachment registry as guest
/// objects. AddDevice executes guest code; provider teardown never substitutes
/// for guest detachment or deletion.
///
//===----------------------------------------------------------------------===//

#include "../DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error pnpDeviceError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PnP device: " + Message);
}
} // namespace

bool KernelModel::isProviderDevice(uint64_t Address) const {
  const auto Found = Devices.find(Address);
  return Found != Devices.end() &&
         Found->second.OwnerKind == DeviceOwnerKind::Provider;
}

KernelModel::PnpDeviceRecord *KernelModel::pnpDeviceForPDO(uint64_t PDO) {
  for (auto &[ID, Device] : PnpDevices) {
    (void)ID;
    if (Device.PDO == PDO)
      return &Device;
  }
  return nullptr;
}

const KernelModel::PnpDeviceRecord *
KernelModel::pnpDeviceForPDO(uint64_t PDO) const {
  for (const auto &[ID, Device] : PnpDevices) {
    (void)ID;
    if (Device.PDO == PDO)
      return &Device;
  }
  return nullptr;
}

llvm::Expected<uint64_t> KernelModel::pnpDeviceForRoute(uint64_t Device) const {
  auto Route = deviceStack(Device);
  if (!Route)
    return Route.takeError();
  uint64_t Identity = 0;
  for (uint64_t Address : *Route) {
    const uint64_t Owner =
        isProviderDevice(Address) ? Address : Devices.at(Address).PnpDevice;
    if (!Owner)
      continue;
    if (!pnpDeviceForPDO(Owner))
      return pnpDeviceError("provider PDO lost its configured identity");
    if (Identity && Identity != Owner)
      return pnpDeviceError("device route crosses configured PDO identities");
    Identity = Owner;
  }
  return Identity;
}

llvm::Error KernelModel::preparePnpDevices() {
  if (!EntryFinished || PnpDevicesPrepared || Unloading || Unloaded)
    return pnpDeviceError("device preparation requires one completed entry");
  DriverOptions Configuration;
  Configuration.PnpDevices = ConfiguredPnpDevices;
  if (auto E = validateDriverScenario(Configuration))
    return E;
  if (auto E = snapshot())
    return E;
  if (!ConfiguredPnpDevices.empty() && !Result.AddDevice)
    return pnpDeviceError(
        "configured PDOs require a registered guest AddDevice callback");
  if (!ConfiguredPnpDevices.empty() && Framework && Framework->hasLiveBinding())
    return pnpDeviceError("KMDF PnP devices are outside this profile");
  if (ConfiguredPnpDevices.empty()) {
    PnpDevicesPrepared = true;
    return llvm::Error::success();
  }
  auto Provider = allocate(DriverObjectSize);
  if (!Provider)
    return Provider.takeError();
  if (auto E = Memory.writeInteger(*Provider, DriverType, 2))
    return E;
  if (auto E = Memory.writeInteger(*Provider + 2, DriverObjectSize, 2))
    return E;
  PnpProviderDriver = *Provider;
  for (const auto &Configured : ConfiguredPnpDevices) {
    // The common scenario validator requires explicit initial states and the
    // resource-free provider. Never supply inferred power or bus responses.
    if (Configured.Bus != DriverBusKind::ResourceFree ||
        !Configured.InitialDevicePower || !Configured.InitialSystemPower)
      return pnpDeviceError("provider requires explicit resource-free states");
    auto Created = createDeviceObjectForOwner(
        {}, 0, UnknownDeviceType, SecureOpen, false, PnpProviderDriver,
        DeviceOwnerKind::Provider);
    if (!Created)
      return Created.takeError();
    if (Created->Status != StatusSuccess)
      return pnpDeviceError("insufficient storage for the provider PDO");
    if (auto E = Lifecycle.addDevice(Created->Address,
                                     *Configured.InitialDevicePower,
                                     *Configured.InitialSystemPower))
      return E;
    const size_t Index = Result.PnpDevices.size();
    Result.PnpDevices.push_back({Configured.ID, Created->Address, std::nullopt,
                                 false, DevicePnpState::NotStarted, true});
    PnpDeviceRecord Record;
    Record.PDO = Created->Address;
    Record.ResultIndex = Index;
    Record.BusResourceFree = true;
    Record.InitialReportedDevicePower = Configured.InitialReportedDevicePower;
    Record.RequestedDevicePower = Configured.RequestedDevicePower;
    Devices.at(Created->Address).ReportedDevicePower =
        Configured.InitialReportedDevicePower;
    auto Flags = Memory.readInteger(Created->Address + DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if (auto E = Memory.writeInteger(Created->Address + DeviceFlagsOffset,
                                     *Flags | DevicePowerPageable, 4))
      return E;
    PnpDevices.emplace(Configured.ID, Record);
  }
  PnpDevicesPrepared = true;
  return snapshot();
}

llvm::Expected<KernelModel::Invocation>
KernelModel::beginAddDevice(llvm::StringRef ID) {
  auto Found = PnpDevices.find(ID.str());
  if (!PnpDevicesPrepared || Found == PnpDevices.end())
    return pnpDeviceError("AddDevice requires a configured PDO identity");
  auto &Device = Found->second;
  if (Device.AddDeviceStatus || Device.AddDeviceActive ||
      std::any_of(PnpDevices.begin(), PnpDevices.end(), [](const auto &Entry) {
        return Entry.second.AddDeviceActive;
      }))
    return pnpDeviceError("AddDevice can execute only once and serially");
  if (!isProviderDevice(Device.PDO) || Unloading || Unloaded ||
      !Requests.empty() || CurrentIRQL != scheduler::PassiveLevel)
    return pnpDeviceError(
        "AddDevice requires a live idle PDO at PASSIVE_LEVEL");
  if (auto E = snapshot())
    return E;
  if (!Result.AddDevice)
    return pnpDeviceError("guest AddDevice callback is missing");
  for (const auto &[Address, Existing] : Devices)
    if (Existing.OwnerKind == DeviceOwnerKind::Guest)
      Device.ExistingGuestDevices.insert(Address);
  Device.AddDeviceActive = true;
  return Invocation{Result.AddDevice, DriverObject, Device.PDO};
}

llvm::Error KernelModel::finishAddDevice(llvm::StringRef ID, uint32_t Status) {
  auto Found = PnpDevices.find(ID.str());
  if (Found == PnpDevices.end() || !Found->second.AddDeviceActive)
    return pnpDeviceError("AddDevice return has no active invocation");
  auto &Device = Found->second;
  Device.AddDeviceActive = false;
  Device.AddDeviceStatus = Status;
  Result.PnpDevices[Device.ResultIndex].AddDeviceStatus = Status;
  if (auto E = snapshot())
    return E;
  if (Status && !(Status & 0x80000000U))
    return pnpDeviceError(
        "AddDevice must return STATUS_SUCCESS or a failing NTSTATUS");
  if (!Status)
    return llvm::Error::success();
  if (std::any_of(Devices.begin(), Devices.end(), [&](const auto &Entry) {
        return Entry.second.OwnerKind == DeviceOwnerKind::Guest &&
               !Device.ExistingGuestDevices.count(Entry.first);
      }))
    return pnpDeviceError("failed AddDevice left a live guest device");
  // A failed AddDevice does not receive a remove IRP. A driver that undid its
  // own work leaves a detached, unreferenced PDO which the provider can retire.
  // Leaked guest objects and callbacks remain visible; never repair them here.
  const auto &PDO = Devices.at(Device.PDO);
  const bool HasReferences =
      PDO.Lower || PDO.Upper || PDO.InternalReferences ||
      Scheduler.hasOutstanding(Device.PDO) ||
      std::any_of(
          WorkItems.begin(), WorkItems.end(),
          [&](const auto &Entry) { return Entry.second == Device.PDO; }) ||
      std::any_of(Files.begin(), Files.end(), [&](const auto &Entry) {
        return Entry.second.Device == Device.PDO;
      });
  if (HasReferences)
    return pnpDeviceError("failed AddDevice left provider references");
  if (auto E = retirePnpProvider(Device.PDO))
    return E;
  return snapshot();
}

llvm::Error KernelModel::retirePnpProvider(uint64_t PDO) {
  const auto *Configured = pnpDeviceForPDO(PDO);
  if (!Configured || Configured->AddDeviceActive)
    return pnpDeviceError("provider teardown lost its inactive PDO identity");
  auto Found = Devices.find(PDO);
  if (Found == Devices.end())
    return llvm::Error::success();
  auto &Device = Found->second;
  if (Device.OwnerKind != DeviceOwnerKind::Provider ||
      Device.OwnerDriver != PnpProviderDriver)
    return pnpDeviceError("provider teardown cannot delete a guest device");
  if (Device.Lower || Device.Upper || Device.InternalReferences ||
      Scheduler.hasOutstanding(PDO) ||
      std::any_of(WorkItems.begin(), WorkItems.end(),
                  [PDO](const auto &Entry) { return Entry.second == PDO; }) ||
      std::any_of(
          Files.begin(), Files.end(),
          [PDO](const auto &Entry) { return Entry.second.Device == PDO; }) ||
      std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        const auto &Request = Entry.second;
        return Request.Device == PDO ||
               std::find(Request.DeviceRoute.begin(), Request.DeviceRoute.end(),
                         PDO) != Request.DeviceRoute.end();
      }))
    return pnpDeviceError(
        "provider teardown requires guest detach and all references to drain");
  if (auto E = validateDeviceTopology())
    return E;
  if (auto E = prepareReleaseRange(PDO, Device.Size))
    return E;
  Device.DeletePending = true;
  if (auto E = retireDeviceIfUnreferenced(PDO))
    return E;
  if (Devices.count(PDO))
    return pnpDeviceError("provider teardown did not retire the PDO");
  return snapshotPnpDevices();
}

llvm::Error KernelModel::finishPnpRemoval(uint64_t PDO) {
  const auto *Configured = pnpDeviceForPDO(PDO);
  if (!Configured)
    return pnpDeviceError("remove finalization requires a configured PDO");
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  if (State->Pnp != DevicePnpState::Removed)
    return pnpDeviceError("provider teardown requires completed PnP removal");
  for (uint64_t Guest : Configured->GuestDevices)
    if (Devices.count(Guest))
      return pnpDeviceError("remove left a live associated guest device");
  return retirePnpProvider(PDO);
}

llvm::Error KernelModel::validatePnpRemovalFinalization(
    const ActiveRequest &Request) const {
  const uint64_t PDO = Request.PnpDevice;
  const auto *Configured = pnpDeviceForPDO(PDO);
  if (!Configured || !isProviderDevice(PDO))
    return pnpDeviceError("remove finalization lost its provider identity");
  auto Inventory = driverDeviceInventory();
  if (!Inventory)
    return Inventory.takeError();
  if (auto E = validateDeviceTopology())
    return E;
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  if (State->Pnp != DevicePnpState::Removed)
    return pnpDeviceError("remove finalization requires completed removal");
  for (uint64_t Guest : Configured->GuestDevices)
    if (Devices.count(Guest) &&
        std::find(Request.DeviceRoute.begin(), Request.DeviceRoute.end(),
                  Guest) == Request.DeviceRoute.end())
      return pnpDeviceError("remove left a live associated guest device");
  for (uint64_t Owner : Request.DeviceRoute) {
    const auto Found = Devices.find(Owner);
    if (Found == Devices.end())
      return pnpDeviceError("remove route lost a retained device");
    const auto &Device = Found->second;
    if (Owner != PDO && !Device.DeletePending)
      return pnpDeviceError("remove returned without deleting a guest device "
                            "in its route");
    if (Device.InternalReferences != 1 || Device.Lower || Device.Upper ||
        Scheduler.hasOutstanding(Owner))
      return pnpDeviceError("remove finalization has a retained guest device "
                            "or attachment");
    for (const auto &[ID, File] : Files) {
      (void)ID;
      if (File.Device == Owner)
        return pnpDeviceError("remove finalization has a retained file");
    }
    for (const auto &[IRP, Other] : Requests)
      if (IRP != Request.IRP &&
          (Other.Device == Owner ||
           std::find(Other.DeviceRoute.begin(), Other.DeviceRoute.end(),
                     Owner) != Other.DeviceRoute.end()))
        return pnpDeviceError("remove finalization has another retained IRP");
    if (Owner == PDO)
      for (const auto &[Item, Device] : WorkItems) {
        (void)Item;
        if (Device == PDO)
          return pnpDeviceError("provider teardown has a live work item");
      }
    if (auto E = canReleaseRange(Owner, Device.Size))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::snapshotPnpDevices() {
  for (const auto &[ID, Device] : PnpDevices) {
    if (Device.ResultIndex >= Result.PnpDevices.size())
      return pnpDeviceError("configured identity lost its report record");
    auto &Observation = Result.PnpDevices[Device.ResultIndex];
    if (Observation.ID != ID || Observation.PDO != Device.PDO)
      return pnpDeviceError("configured identity does not match its report");
    const auto Found = Devices.find(Device.PDO);
    Observation.ProviderPresent = Found != Devices.end();
    Observation.Attached = Found != Devices.end() && Found->second.Upper;
    Observation.AddDeviceStatus = Device.AddDeviceStatus;
    auto State = Lifecycle.snapshot(Device.PDO);
    if (!State)
      return State.takeError();
    Observation.PnpState = State->Pnp;
    Observation.DevicePower = State->DevicePower;
    Observation.SystemPower = State->SystemPower;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
