//===- KernelResources.cpp - Physical resource assignment epochs   -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Advance physical assignment state only from actual provider completion.
/// Resource consumers share one epoch and physical availability decision.
///
//===----------------------------------------------------------------------===//

#include "KernelResources.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>
#include <limits>

namespace neverd::emulation {
namespace {
llvm::Error resourceError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "resources: " + Message);
}
bool failed(uint32_t Status) { return Status & profile::NTStatusFailureMask; }
} // namespace

llvm::Error KernelResources::configure(uint64_t PDO,
                                       const DriverPnpDevice &Configuration) {
  if (Configuration.Resources.empty() && Configuration.Interrupts.empty() &&
      !Configuration.Dma)
    return llvm::Error::success();
  if (!PDO || Devices.count(PDO) || !Configuration.InitialDevicePower ||
      Configuration.Bus != DriverBusKind::RegisterBank)
    return resourceError("invalid or duplicate resource provider");
  Device Record;
  Record.ID = Configuration.ID;
  Record.Memory = Configuration.Resources;
  Record.Interrupts = Configuration.Interrupts;
  Record.Dma = Configuration.Dma;
  Record.Power = *Configuration.InitialDevicePower;
  Devices.emplace(PDO, std::move(Record));
  return llvm::Error::success();
}

bool KernelResources::hasResources(uint64_t PDO) const {
  return Devices.count(PDO);
}

llvm::Expected<std::vector<uint8_t>>
KernelResources::resourceList(uint64_t PDO, bool Translated) const {
  const auto It = Devices.find(PDO);
  if (It == Devices.end())
    return resourceError("resource list requires a configured register bank");
  const auto &Resources = It->second.Memory;
  const auto &Interrupts = It->second.Interrupts;
  std::vector<uint8_t> Bytes(resources::ResourceHeaderSize +
                             (Resources.size() + Interrupts.size()) *
                                 resources::ResourceDescriptorSize);
  auto Put = [&](uint64_t Offset, uint64_t Value, unsigned Size) {
    for (unsigned I = 0; I < Size; ++I)
      Bytes[Offset + I] = uint8_t(Value >> (I * 8));
  };
  Put(resources::ResourceCountOffset, resources::SupportedFullDescriptorCount,
      resources::ResourceCountFieldSize);
  Put(resources::ResourceInterfaceOffset, resources::InterfaceInternal, 4);
  Put(resources::ResourceBusOffset, 0, 4);
  Put(resources::ResourceVersionOffset, 1, 2);
  Put(resources::ResourceRevisionOffset, 1, 2);
  Put(resources::ResourcePartialCountOffset,
      Resources.size() + Interrupts.size(), resources::ResourceCountFieldSize);
  for (size_t I = 0; I < Resources.size(); ++I) {
    const auto &Resource = Resources[I];
    const uint64_t Base =
        resources::ResourceHeaderSize + I * resources::ResourceDescriptorSize;
    Put(Base + resources::ResourceTypeOffset, resources::MemoryType, 1);
    Put(Base + resources::ResourceShareOffset, resources::DeviceExclusive, 1);
    Put(Base + resources::ResourceFlagsOffset, resources::MemoryReadWrite, 2);
    Put(Base + resources::ResourceStartOffset,
        Translated ? Resource.TranslatedStart : Resource.RawStart, 8);
    Put(Base + resources::ResourceLengthOffset, Resource.Length, 4);
  }
  for (size_t I = 0; I < Interrupts.size(); ++I) {
    const auto &Interrupt = Interrupts[I];
    const uint64_t Base =
        resources::ResourceHeaderSize +
        (Resources.size() + I) * resources::ResourceDescriptorSize;
    Put(Base + resources::ResourceTypeOffset, resources::InterruptType, 1);
    Put(Base + resources::ResourceShareOffset, uint8_t(Interrupt.Share), 1);
    Put(Base + resources::ResourceFlagsOffset,
        Interrupt.Mode == DriverInterruptMode::Latched
            ? resources::InterruptLatched
            : resources::InterruptLevelSensitive,
        2);
    Put(Base + resources::InterruptLevelOffset,
        Translated ? Interrupt.TranslatedLevel : Interrupt.RawLevel, 4);
    Put(Base + resources::InterruptVectorOffset,
        Translated ? Interrupt.TranslatedVector : Interrupt.RawVector, 4);
    Put(Base + resources::InterruptAffinityOffset,
        Translated ? Interrupt.TranslatedAffinity : Interrupt.RawAffinity, 8);
  }
  return Bytes;
}

llvm::Error KernelResources::canStart(uint64_t PDO) const {
  const auto It = Devices.find(PDO);
  if (It == Devices.end())
    return llvm::Error::success();
  const auto &Device = It->second;
  if (!Device.Present || Device.Assigned || Device.Starting ||
      Device.Epoch == UINT64_MAX)
    return resourceError("START requires a present unassigned resource epoch");
  return StartCheck(PDO);
}

llvm::Error KernelResources::beginStart(uint64_t PDO) {
  if (auto E = canStart(PDO))
    return E;
  const auto It = Devices.find(PDO);
  if (It != Devices.end()) {
    ++It->second.Epoch;
    It->second.Starting = true;
  }
  return llvm::Error::success();
}

llvm::Error KernelResources::completeLowerStart(uint64_t PDO, uint32_t Status) {
  const auto It = Devices.find(PDO);
  if (It == Devices.end())
    return llvm::Error::success();
  auto &Device = It->second;
  if (!Device.Starting || !Device.Present || Device.Assigned)
    return resourceError("lower START lost its pending resource epoch");
  Device.Assigned = !failed(Status);
  return llvm::Error::success();
}

llvm::Error KernelResources::validateCompletion(uint64_t PDO,
                                                DevicePnpRequest Minor,
                                                uint32_t Status) const {
  const auto It = Devices.find(PDO);
  if (It == Devices.end())
    return llvm::Error::success();
  const auto &Device = It->second;
  if (Minor == DevicePnpRequest::Start) {
    if (!Device.Starting || (!failed(Status) && !Device.Assigned))
      return resourceError(
          "START completion requires its actual resource epoch");
    if (failed(Status))
      return Check(PDO);
  }
  if ((Minor == DevicePnpRequest::Stop || Minor == DevicePnpRequest::Remove) &&
      !failed(Status))
    return Check(PDO);
  return llvm::Error::success();
}

llvm::Error KernelResources::finishPnp(uint64_t PDO, DevicePnpRequest Minor,
                                       uint32_t Status) {
  if (auto E = validateCompletion(PDO, Minor, Status))
    return E;
  const auto It = Devices.find(PDO);
  if (It == Devices.end())
    return llvm::Error::success();
  auto &Device = It->second;
  if (Minor == DevicePnpRequest::Start) {
    Device.Starting = false;
    if (failed(Status))
      Device.Assigned = false;
  } else if (!failed(Status) && (Minor == DevicePnpRequest::Stop ||
                                 Minor == DevicePnpRequest::Remove)) {
    Device.Assigned = false;
    if (Minor == DevicePnpRequest::Remove)
      Device.Present = false;
  }
  return llvm::Error::success();
}

void KernelResources::surpriseRemoval(uint64_t PDO) {
  const auto It = Devices.find(PDO);
  if (It != Devices.end())
    It->second.Present = false;
}

void KernelResources::setPhysicalPower(uint64_t PDO, DevicePowerState Power) {
  const auto It = Devices.find(PDO);
  if (It != Devices.end())
    It->second.Power = Power;
}

const KernelResources::Device *KernelResources::find(uint64_t PDO) const {
  const auto It = Devices.find(PDO);
  return It == Devices.end() ? nullptr : &It->second;
}

} // namespace neverd::emulation
