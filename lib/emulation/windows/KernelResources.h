//===- KernelResources.h - Physical resource assignment authority --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own resource packets, epochs, physical presence and power independently of
/// driver notifications, virtual mappings and interrupt connections.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELRESOURCES_H
#define NEVERD_EMULATION_KERNELRESOURCES_H

#include "neverd/emulation/DriverPnp.h"

#include "llvm/Support/Error.h"

#include <functional>
#include <map>

namespace neverd::emulation {
namespace resources {
#define NEVERD_KERNEL_RESOURCE_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#include "KernelResourceValues.def"
#undef NEVERD_KERNEL_RESOURCE_VALUE
} // namespace resources
class KernelResources {
public:
  /// The coordinator checks all live resource consumers before assignment
  /// release. This callback is pure and must outlive this authority.
  using CheckRelease = std::function<llvm::Error(uint64_t)>;
  explicit KernelResources(CheckRelease Release, CheckRelease Start = {})
      : Check(std::move(Release)),
        StartCheck(Start ? std::move(Start) : Check) {}
  KernelResources(const KernelResources &) = delete;
  KernelResources &operator=(const KernelResources &) = delete;
  struct Device {
    std::string ID;
    std::vector<DriverMemoryResource> Memory;
    std::vector<DriverInterruptResource> Interrupts;
    std::optional<DriverDmaConfig> Dma;
    uint64_t Epoch = 0;
    bool Starting = false;
    bool Assigned = false;
    bool Present = true;
    DevicePowerState Power = DevicePowerState::D0;
  };
  const Device *find(uint64_t PDO) const;
  const std::map<uint64_t, Device> &devices() const { return Devices; }
  llvm::Error configure(uint64_t PDO, const DriverPnpDevice &Configuration);
  bool hasResources(uint64_t PDO) const;
  llvm::Expected<std::vector<uint8_t>> resourceList(uint64_t PDO,
                                                    bool Translated) const;
  llvm::Error canStart(uint64_t PDO) const;
  llvm::Error beginStart(uint64_t PDO);
  llvm::Error completeLowerStart(uint64_t PDO, uint32_t Status);
  llvm::Error validateCompletion(uint64_t PDO, DevicePnpRequest Minor,
                                 uint32_t Status) const;
  llvm::Error finishPnp(uint64_t PDO, DevicePnpRequest Minor, uint32_t Status);
  void surpriseRemoval(uint64_t PDO);
  void setPhysicalPower(uint64_t PDO, DevicePowerState Power);

private:
  CheckRelease Check;
  CheckRelease StartCheck;
  std::map<uint64_t, Device> Devices;
};
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELRESOURCES_H
