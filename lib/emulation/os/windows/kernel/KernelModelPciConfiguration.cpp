///
/// \file
/// KernelModelPciConfiguration.
///

#include "KernelModel.h"

#include <algorithm>
namespace neverd::emulation {
llvm::Expected<uint64_t> KernelModel::pciBusData(llvm::ArrayRef<uint64_t> A,
                                                 bool Writing) {
  auto Unsupported = [](const llvm::Twine &Text) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "PCI configuration: " + Text);
  };
  const uint32_t Type = uint32_t(A[0]), Bus = uint32_t(A[1]),
                 Slot = uint32_t(A[2]);
  if (!Type)
    return Unsupported("CMOS bus data requires its own provider");
  if (Type != 4)
    return uint64_t(0);
  if (!PciConfiguration)
    return Unsupported("requires pci_configuration");
  auto &P = *PciConfiguration;
  if (!Writing && (Bus < P.MinimumBusNumber || Bus > P.MaximumBusNumber))
    return uint64_t(0);
  if (Slot > 255 || Bus > 0xffffff)
    return Unsupported("reserved bus/slot bits are unsupported");
  const uint32_t Offset = uint32_t(A[4]),
                 Length = std::min<uint32_t>(uint32_t(A[5]),
                                             DriverPciConfigurationBytes);
  if (!Length)
    return uint64_t(0);
  if (Offset >= DriverPciConfigurationBytes ||
      Length > DriverPciConfigurationBytes - Offset)
    return Unsupported(
        "access exceeds the supplied function configuration window");
  auto Device =
      std::find_if(P.Devices.begin(), P.Devices.end(), [&](const auto &D) {
        return uint32_t((uint32_t(D.Segment) << 8) | D.Bus) == Bus &&
               uint32_t(D.Device | (uint32_t(D.Function) << 5)) == Slot;
      });
  const bool Present = Device != P.Devices.end();
  if (Writing &&
      (!Present || (Offset < 64 && (Device->Configuration[14] & 0x7f))))
    return uint64_t(0);
  const uint32_t Count = Present      ? Length
                         : Offset < 2 ? std::min<uint32_t>(Length, 2 - Offset)
                                      : 0;
  if (!Count)
    return uint64_t(0);
  if (Writing && !Device->WriteMask)
    return Unsupported("requires both function write masks");
  if (A[3] < profile::UserProbeLimit)
    return Unsupported("buffer must be kernel nonpaged RAM");
  if (auto E = objectBuffer(A[3], Count, !Writing))
    return E;
  for (const auto &[Base, Allocation] : Allocations)
    if (!Allocation.NonPaged && A[3] < Base + Allocation.Size &&
        Base < A[3] + Count)
      return Unsupported("buffer must be nonpaged");
  auto Pin = Memory.pinBacking(A[3], Count);
  if (!Pin)
    return Pin.takeError();
  if (!Writing) {
    std::vector<uint8_t> Bytes(Count, 255);
    if (Present)
      std::copy_n(Device->Configuration.begin() + Offset, Count, Bytes.begin());
    if (auto E = Memory.write(A[3], Bytes))
      return E;
  } else {
    std::vector<uint8_t> Bytes(Count);
    if (auto E = Memory.read(A[3], Bytes))
      return E;
    for (size_t I = 0; I < Count; ++I) {
      const size_t At = Offset + I;
      const uint8_t R = (*Device->WriteMask)[At],
                    C = (*Device->WriteOneToClearMask)[At],
                    Old = Device->Configuration[At];
      Device->Configuration[At] =
          uint8_t((Old & uint8_t(~(R | C))) | (Bytes[I] & R) |
                  ((Old & C) & uint8_t(~Bytes[I])));
    }
  }
  return uint64_t(Count);
}
} // namespace neverd::emulation
