//===- KernelMMIO.h - Resource epochs and physical register banks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own explicit synthetic physical registers independently of virtual aliases,
/// top-level PnP state and driver power notifications.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMMIO_H
#define NEVERD_EMULATION_KERNELMMIO_H

#include "../GuestMemory.h"

#include "neverd/emulation/DriverPnp.h"

#include <map>
#include <memory>
#include <vector>

namespace neverd::emulation {
namespace mmio {
#define NEVERD_KERNEL_MMIO_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelMMIOValues.def"
#undef NEVERD_KERNEL_MMIO_VALUE
} // namespace mmio

class KernelMMIO {
public:
  explicit KernelMMIO(GuestMemory &Memory) : Memory(Memory) {}
  KernelMMIO(const KernelMMIO &) = delete;
  KernelMMIO &operator=(const KernelMMIO &) = delete;
  /// Configuration is validated collectively by validateDriverResources.
  llvm::Error configure(uint64_t PDO, const DriverPnpDevice &Configuration);
  bool hasResources(uint64_t PDO) const;
  llvm::Expected<std::vector<uint8_t>> resourceList(uint64_t PDO,
                                                    bool Translated) const;
  /// Pure preparation precedes a new START ticket; activation is lower success.
  llvm::Error canStart(uint64_t PDO) const;
  llvm::Error beginStart(uint64_t PDO);
  llvm::Error completeLowerStart(uint64_t PDO, uint32_t Status);
  llvm::Error validateCompletion(uint64_t PDO, DevicePnpRequest Minor,
                                 uint32_t Status) const;
  llvm::Error finishPnp(uint64_t PDO, DevicePnpRequest Minor, uint32_t Status);
  void surpriseRemoval(uint64_t PDO);
  void setPhysicalPower(uint64_t PDO, DevicePowerState Power);
  llvm::Error canRemove(uint64_t PDO) const;
  llvm::Expected<uint64_t> map(uint64_t Physical, uint64_t Length,
                               uint32_t Attributes, bool Extended);
  llvm::Error unmap(uint64_t Address, uint64_t Length);

private:
  struct Device {
    std::vector<DriverMemoryResource> Resources;
    uint64_t Epoch = 0;
    bool Starting = false;
    bool Assigned = false;
    bool Present = true;
    DevicePowerState Power = DevicePowerState::D0;
  };
  struct Mapping {
    uint64_t PDO;
    size_t ResourceIndex;
    uint64_t Epoch;
    uint64_t Address;
    uint64_t Length;
    uint64_t PageBase;
    uint64_t PageSize;
    uint64_t Physical;
    bool ReadOnly;
  };
  GuestMemory &Memory;
  std::shared_ptr<unsigned char> Lifetime = std::make_shared<unsigned char>(0);
  std::map<uint64_t, Device> Devices;
  std::map<uint64_t, Mapping> Mappings;
  uint64_t NextMapping = 0;
  llvm::Error noMappings(uint64_t PDO) const;
  llvm::Error validate(uint64_t Address, uint64_t Offset, uint64_t Size,
                       bool Write) const;
  llvm::Expected<uint64_t> read(uint64_t Address, uint64_t Offset,
                                unsigned Size);
  llvm::Error write(uint64_t Address, uint64_t Offset, unsigned Size,
                    uint64_t Value);
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELMMIO_H
