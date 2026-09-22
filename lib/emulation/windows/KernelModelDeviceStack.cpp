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

llvm::Error KernelModel::validateDeviceTopology() const {
  for (const auto &[Address, Device] : Devices) {
    if (Device.Address != Address || Device.OwnerDriver != DriverObject)
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
      auto Value = Memory.readInteger(Address + F.Offset, F.Width);
      if (!Value)
        return Value.takeError();
      if (*Value != F.Value)
        return deviceError(
            "DEVICE_OBJECT identity or attachment was corrupted");
    }
    auto Count = Memory.readInteger(Address + DeviceStackCountOffset, 1);
    if (!Count)
      return Count.takeError();
    if (!*Count || *Count > MaxIRPStackCount)
      return deviceError("DEVICE_OBJECT.StackSize must be a positive CCHAR");
    if (Device.Lower) {
      auto Lower = Devices.find(Device.Lower);
      if (Lower == Devices.end() || Lower->second.Upper != Address)
        return deviceError("device attachment has no matching lower neighbor");
      auto LowerCount =
          Memory.readInteger(Device.Lower + DeviceStackCountOffset, 1);
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

llvm::Error
KernelModel::validateDeviceStackMutation(llvm::ArrayRef<uint64_t> Stack) const {
  for (uint64_t Address : Stack) {
    const auto &Device = Devices.at(Address);
    if (FrameworkDevices.count(Address))
      return deviceError("framework device attachment is outside this profile");
    if (Device.InternalReferences || Scheduler.hasOutstanding(Address) ||
        std::any_of(Files.begin(), Files.end(),
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
