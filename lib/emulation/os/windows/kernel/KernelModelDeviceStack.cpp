//===- KernelModelDeviceStack.cpp - WDM device ownership and attachments --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep device ownership, attachment topology and internal holds distinct.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <set>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error deviceError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

} // namespace

llvm::Expected<uint64_t> KernelModel::readObjectField(uint64_t Address,
                                                      unsigned Width,
                                                      ObjectRead Mode) const {
  if (Mode == ObjectRead::Execution)
    return Memory.readInteger(Address, Width);
  if (Width != 1 && Width != 2 && Width != 4 && Width != 8)
    return deviceError("invalid kernel object scalar width");
  std::array<uint8_t, 8> Bytes{};
  if (auto E = Memory.snapshotBacking(
          Address, llvm::MutableArrayRef(Bytes).take_front(Width)))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes.data());
}

llvm::Expected<std::vector<uint64_t>>
KernelModel::driverDeviceInventory(ObjectRead Mode) const {
  std::vector<uint64_t> Inventory;
  std::set<uint64_t> Seen;
  std::vector<uint64_t> Owners{DriverObject, PnpProviderDriver,
                               FiniteFileSystemDriver,
                               FileSystemProviderDriver};
  for (const auto &[Key, D] : NamedDrivers)
    if (D.Guest && D.Object != DriverObject)
      Owners.push_back(D.Object);
  for (uint64_t Owner : Owners) {
    if (!Owner)
      continue;
    auto Head = readObjectField(Owner + DriverDeviceHead, 8, Mode);
    if (!Head)
      return Head.takeError();
    uint64_t Current = *Head;
    while (Current) {
      const auto Found = Devices.find(Current);
      if (Found == Devices.end() || Found->second.OwnerDriver != Owner ||
          Found->second.UnlinkedFromDriver || !Seen.insert(Current).second)
        return deviceError(
            "unknown device or cycle in DRIVER_OBJECT device list");
      Inventory.push_back(Current);
      auto Next = readObjectField(Current + DeviceNext, 8, Mode);
      if (!Next)
        return Next.takeError();
      Current = *Next;
    }
  }
  if (Seen.size() != size_t(std::count_if(
                         Devices.begin(), Devices.end(), [](const auto &Entry) {
                           return !Entry.second.UnlinkedFromDriver;
                         })))
    return deviceError("live device detached from DRIVER_OBJECT device list");
  return Inventory;
}

llvm::Error KernelModel::validateDeviceTopology(ObjectRead Mode) const {
  for (const auto &[Address, Device] : Devices) {
    const uint64_t ExpectedOwner =
        Device.OwnerKind == DeviceOwnerKind::Guest
            ? (isGuestDriverObject(Device.OwnerDriver) ? Device.OwnerDriver : 0)
            : deviceOwnerDriver(Device.OwnerKind);
    if (Device.Address != Address || !ExpectedOwner ||
        Device.OwnerDriver != ExpectedOwner)
      return deviceError("device has an unsupported driver owner");
    struct Field {
      uint64_t Offset;
      uint64_t Value;
      unsigned Width;
    };
    for (const auto &F :
         std::array<Field, 6>{{{0, DeviceType, 2},
                               {ObjectSizeOffset, Device.Size, 2},
                               {DeviceDriverOffset, Device.OwnerDriver, 8},
                               {DeviceExtensionOffset, Device.Extension, 8},
                               {DeviceTypeOffset, Device.Type, 4},
                               {DeviceAttachedOffset, Device.Upper, 8}}}) {
      auto Value = readObjectField(Address + F.Offset, F.Width, Mode);
      if (!Value)
        return Value.takeError();
      if (*Value != F.Value)
        return deviceError(
            "DEVICE_OBJECT identity or attachment was corrupted");
    }
    auto Count = readObjectField(Address + DeviceStackCountOffset, 1, Mode);
    if (!Count)
      return Count.takeError();
    if (!*Count || *Count > MaxIRPStackCount)
      return deviceError("DEVICE_OBJECT.StackSize must be a positive CCHAR");
    auto Flags = readObjectField(Address + DeviceFlagsOffset, 4, Mode);
    if (!Flags)
      return Flags.takeError();
    if (bool(*Flags & DeviceBusEnumerated) !=
        (Device.OwnerKind == DeviceOwnerKind::PnpProvider))
      return deviceError("DEVICE_OBJECT bus-enumerated role was corrupted");
    if (Device.OwnerKind != DeviceOwnerKind::Guest && Device.Lower)
      return deviceError(
          "terminal provider cannot attach above another device");
    if (Device.OwnerKind == DeviceOwnerKind::FileSystemProvider)
      if (auto E = validateFileSystemDevice(Address, Mode))
        return E;
    if (bool(*Flags & DeviceShutdownRegistered) !=
        bool(Device.ShutdownNotifications))
      return deviceError(
          "DEVICE_OBJECT shutdown registration flag was corrupted");
    if (Device.Lower) {
      auto Lower = Devices.find(Device.Lower);
      if (Lower == Devices.end() || Lower->second.Upper != Address)
        return deviceError("device attachment has no matching lower neighbor");
      auto LowerCount =
          readObjectField(Device.Lower + DeviceStackCountOffset, 1, Mode);
      if (!LowerCount)
        return LowerCount.takeError();
      if (*Count <= *LowerCount)
        return deviceError(
            "DEVICE_OBJECT.StackSize cannot hold its lower stack");
    }
    if (Device.Upper) {
      auto Upper = Devices.find(Device.Upper);
      if (Upper == Devices.end() || Upper->second.Lower != Address)
        return deviceError("device attachment has no matching upper neighbor");
    }
  }
  // Strictly increasing, positive, bounded StackSize values already exclude
  // cycles. Walk both directions below when resolving a particular stack.
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::topAttachedDevice(uint64_t Base) const {
  if (auto E = validateDeviceTopology())
    return E;
  std::set<uint64_t> Seen;
  uint64_t Current = Base;
  while (true) {
    auto Device = Devices.find(Current);
    if (Device == Devices.end() || !Seen.insert(Current).second)
      return deviceError("unknown device or cycle in attachment stack");
    if (!Device->second.Upper)
      return Current;
    Current = Device->second.Upper;
  }
}

llvm::Expected<std::vector<uint64_t>>
KernelModel::deviceStack(uint64_t Top) const {
  if (auto E = validateDeviceTopology())
    return E;
  std::vector<uint64_t> Stack;
  std::set<uint64_t> Seen;
  uint64_t Current = Top;
  while (Current) {
    auto Device = Devices.find(Current);
    if (Device == Devices.end() || !Seen.insert(Current).second)
      return deviceError("unknown device or cycle in attachment stack");
    Stack.push_back(Current);
    Current = Device->second.Lower;
  }
  if (Stack.empty())
    return deviceError("device stack requires a live top device");
  return Stack;
}

llvm::Expected<uint64_t> KernelModel::queryDeviceStack(uint64_t Device,
                                                       DeviceStackQuery Query) {
  uint64_t Selected = 0;
  if (Query == DeviceStackQuery::Top ||
      Query == DeviceStackQuery::TopReference) {
    auto Top = topAttachedDevice(Device);
    if (!Top)
      return Top.takeError();
    Selected = *Top;
  } else {
    auto Stack = deviceStack(Device);
    if (!Stack)
      return Stack.takeError();
    if (Query == DeviceStackQuery::BaseReference) {
      Selected = Stack->back();
    } else {
      // The target lower-device query checks the input object's deletion
      // state, not the selected neighbor's. IoDeleteDevice sets that state;
      // attachment and pointer references can still retain either object.
      if (Devices.at(Device).DeletePending)
        return 0;
      Selected = Devices.at(Device).Lower;
    }
  }
  if (Selected && Query != DeviceStackQuery::Top) {
    auto Referenced = referenceObject(Selected);
    if (!Referenced)
      return Referenced.takeError();
  }
  return Selected;
}

llvm::Error
KernelModel::validateDeviceStackMutation(llvm::ArrayRef<uint64_t> Stack) const {
  for (uint64_t Address : Stack) {
    const auto &Device = Devices.at(Address);
    if (FrameworkDevices.count(Address))
      return deviceError("framework device attachment is outside this profile");
    if (Device.InternalReferences || Scheduler.hasOutstanding(Address) ||
        std::any_of(FileObjects.begin(), FileObjects.end(),
                    [&](const auto &Entry) {
                      return Entry.second.Address &&
                             Entry.second.Device == Address;
                    }) ||
        std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          return Entry.second.Device == Address;
        }))
      return deviceError(
          "attaching to a device stack with live files or callbacks is outside "
          "this profile");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::attachDevice(uint64_t Source,
                                                   uint64_t Target) {
  if (auto E = validateDeviceTopology())
    return E;
  auto SourceIt = Devices.find(Source);
  auto TargetIt = Devices.find(Target);
  if (SourceIt == Devices.end() || TargetIt == Devices.end())
    return deviceError("IoAttachDeviceToDeviceStack requires live devices");
  if (SourceIt->second.OwnerKind != DeviceOwnerKind::Guest)
    return deviceError(
        "IoAttachDeviceToDeviceStack source must be guest-owned");
  if (Source == Target)
    return deviceError("IoAttachDeviceToDeviceStack cannot attach a device to "
                       "itself");
  if (SourceIt->second.Lower || SourceIt->second.Upper)
    return deviceError("IoAttachDeviceToDeviceStack source must be unattached");
  auto Top = topAttachedDevice(Target);
  if (!Top)
    return Top.takeError();
  auto Stack = deviceStack(*Top);
  if (!Stack)
    return Stack.takeError();
  auto PnpOwner = pnpDeviceForRoute(*Top);
  if (!PnpOwner)
    return PnpOwner.takeError();
  if (SourceIt->second.PnpDevice && SourceIt->second.PnpDevice != *PnpOwner)
    return deviceError("reattaching a device to another PnP identity is "
                       "outside this profile");
  if (*PnpOwner &&
      (!isPnpProviderDevice(Stack->back()) || Stack->back() != *PnpOwner))
    return deviceError("PnP attachment target no longer reaches its PDO");
  Stack->push_back(Source);
  for (uint64_t Address : *Stack)
    if (Devices.at(Address).DeletePending)
      return 0;
  if (auto E = validateDeviceStackMutation(*Stack))
    return E;
  auto Count = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!Count)
    return Count.takeError();
  auto Alignment = Memory.readInteger(*Top + DeviceAlignmentOffset, 4);
  if (!Alignment)
    return Alignment.takeError();
  if (*Count == MaxIRPStackCount)
    return deviceError(
        "IoAttachDeviceToDeviceStack exceeds StackSize capacity");

  struct Field {
    uint64_t Address;
    uint64_t Value;
    unsigned Width;
  };
  const std::array<Field, 3> Fields = {
      {{Source + DeviceStackCountOffset, *Count + 1, 1},
       {Source + DeviceAlignmentOffset, *Alignment, 4},
       {*Top + DeviceAttachedOffset, Source, 8}}};
  // Probe each model-owned field without changing bytes. A permission fault
  // must not publish one attachment edge before discovering another is
  // unwritable.
  for (const auto &F : Fields) {
    auto Old = Memory.readInteger(F.Address, F.Width);
    if (!Old)
      return Old.takeError();
    if (auto E = Memory.writeInteger(F.Address, *Old, F.Width))
      return E;
  }
  for (const auto &F : Fields)
    if (auto E = Memory.writeInteger(F.Address, F.Value, F.Width))
      return E;
  SourceIt->second.Lower = *Top;
  Devices.at(*Top).Upper = Source;
  if (*PnpOwner) {
    if (!SourceIt->second.PnpDevice)
      SourceIt->second.ReportedDevicePower =
          pnpDeviceForPDO(*PnpOwner)->InitialReportedDevicePower;
    SourceIt->second.PnpDevice = *PnpOwner;
    pnpDeviceForPDO(*PnpOwner)->GuestDevices.insert(Source);
  }
  return *Top;
}

llvm::Error KernelModel::detachDevice(uint64_t Lower) {
  if (auto E = validateDeviceTopology())
    return E;
  auto LowerIt = Devices.find(Lower);
  if (LowerIt == Devices.end() || !LowerIt->second.Upper)
    return deviceError("IoDetachDevice requires the saved lower attachment");
  const uint64_t Source = LowerIt->second.Upper;
  auto &Upper = Devices.at(Source);
  if (Upper.Upper)
    return deviceError("IoDetachDevice of an intermediate layer is outside "
                       "this profile");
  if (FrameworkDevices.count(Lower) || FrameworkDevices.count(Source))
    return deviceError("framework device attachment is outside this profile");
  if (auto E = canReleaseRemoveLockStorage(Source, Upper.Size))
    return E;
  if (auto E = Memory.writeInteger(Lower + DeviceAttachedOffset, 0, 8))
    return E;
  LowerIt->second.Upper = 0;
  Upper.Lower = 0;
  // Detachment does not shrink a driver's reserved stack capacity. Existing
  // packets retain their captured device route independently of these links.
  if (auto E = retireDeviceIfUnreferenced(Source))
    return E;
  return retireDeviceIfUnreferenced(Lower);
}

llvm::Error KernelModel::detachFrameworkPnpDevice(uint64_t Source,
                                                  uint64_t PDO) {
  if (auto E = validateDeviceTopology())
    return E;
  auto FDO = Devices.find(Source);
  auto Physical = Devices.find(PDO);
  if (FDO == Devices.end() || Physical == Devices.end() ||
      !FrameworkDevices.count(Source) || !isPnpProviderDevice(PDO) ||
      FDO->second.PnpDevice != PDO || FDO->second.Lower != PDO ||
      FDO->second.Upper || Physical->second.Upper != Source)
    return deviceError("framework PnP detach requires its direct FDO/PDO pair");
  if (auto E = canReleaseRemoveLockStorage(Source, FDO->second.Size))
    return E;
  if (auto E = Memory.writeInteger(PDO + DeviceAttachedOffset, 0, 8))
    return E;
  Physical->second.Upper = 0;
  FDO->second.Lower = 0;
  // The captured REMOVE route retains both objects until its own finalization.
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::shutdownNotification(uint64_t Address,
                                                           bool Register) {
  auto Device = Devices.find(Address);
  if (Device == Devices.end() || Device->second.DeletePending ||
      Device->second.OwnerKind != DeviceOwnerKind::Guest)
    return deviceError(
        "shutdown notification requires a live guest-owned device");
  if (auto E = validateDeviceTopology())
    return E;
  if (!Register) {
    if (!Device->second.ShutdownNotifications)
      return deviceError("IoUnregisterShutdownNotification requires a prior "
                         "registration for this device");
    if (auto E = clearShutdownNotifications(Address))
      return E;
    return uint64_t(0); // VOID ABI, not an NTSTATUS result.
  }
  uint64_t Entries = 0;
  for (const auto &[Object, Record] : Devices)
    Entries += Record.ShutdownNotifications;
  if (Entries >= profile::MaxConcurrentCallbacks)
    return uint64_t(StatusInsufficientResources);
  auto Flags = Memory.readInteger(Address + DeviceFlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (auto E = Memory.writeInteger(Address + DeviceFlagsOffset,
                                   *Flags | DeviceShutdownRegistered, 4))
    return E;
  ++Device->second.ShutdownNotifications;
  // Registration queues a device; it does not execute DispatchShutdown.
  // Shutdown delivery is not yet admitted by the scenario request provider.
  return uint64_t(StatusSuccess);
}

llvm::Error KernelModel::clearShutdownNotifications(uint64_t Address) {
  auto Device = Devices.find(Address);
  if (Device == Devices.end())
    return deviceError(
        "cannot clear shutdown notifications for an unknown device");
  auto Flags = Memory.readInteger(Address + DeviceFlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (auto E = Memory.writeInteger(Address + DeviceFlagsOffset,
                                   *Flags & ~DeviceShutdownRegistered, 4))
    return E;
  Device->second.ShutdownNotifications = 0;
  return llvm::Error::success();
}

llvm::Error KernelModel::retainDevice(uint64_t Address) {
  auto Device = Devices.find(Address);
  if (Device == Devices.end())
    return deviceError("cannot retain an unknown device");
  if (Device->second.InternalReferences == UINT64_MAX)
    return deviceError("device internal reference count overflow");
  ++Device->second.InternalReferences;
  return llvm::Error::success();
}

llvm::Error KernelModel::releaseDevice(uint64_t Address) {
  auto Device = Devices.find(Address);
  if (Device == Devices.end() || !Device->second.InternalReferences)
    return deviceError("device internal reference count underflow");
  --Device->second.InternalReferences;
  return retireDeviceIfUnreferenced(Address);
}
} // namespace neverd::emulation
