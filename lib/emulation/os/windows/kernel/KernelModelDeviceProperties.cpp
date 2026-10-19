//===- KernelModelDeviceProperties.cpp - PDO setup-property query ---------===//
///
/// \file
/// PDO setup-property query.
///

#include "KernelModel.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::queryDeviceProperty(llvm::ArrayRef<uint64_t> A) {
  auto Unsupported = [](const llvm::Twine &Text) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "IoGetDeviceProperty: " + Text);
  };
  const uint32_t Property = uint32_t(A[1]) & 0xfff, Capacity = uint32_t(A[2]);
  if (!A[4])
    return Unsupported("requires a writable ResultLength");
  auto Writable = [&](uint64_t Address, uint32_t Size) -> llvm::Error {
    if (auto E = objectBuffer(Address, Size, true))
      return E;
    auto Pin = Memory.pinBacking(Address, Size);
    if (!Pin)
      return Pin.takeError();
    return llvm::Error::success();
  };
  if (auto E = Writable(A[4], 4))
    return E;
  const auto Device = Devices.find(A[0]);
  if (A[0] && (Device == Devices.end() || Device->second.DeletePending))
    return Unsupported("requires a live known device object");
  if (!A[0] || !isPnpProviderDevice(A[0])) {
    if (auto E = Memory.writeInteger(A[4], 0, 4))
      return E;
    return uint64_t(0xc0000010); // STATUS_INVALID_DEVICE_REQUEST
  }
  if (Property > 22) {
    if (auto E = Memory.writeInteger(A[4], 0, 4))
      return E;
    return uint64_t(0xc00000f0); // STATUS_INVALID_PARAMETER_2
  }
  const auto Configured =
      std::find_if(ConfiguredPnpDevices.begin(), ConfiguredPnpDevices.end(),
                   [&](const auto &P) {
                     const auto It = PnpDevices.find(P.ID);
                     return It != PnpDevices.end() && It->second.PDO == A[0];
                   });
  if (Configured == ConfiguredPnpDevices.end() || !Configured->DeviceProperties)
    return Unsupported("requires pnp_devices[].device_properties");
  const auto &Properties = *Configured->DeviceProperties;
  const auto Value =
      std::find_if(Properties.begin(), Properties.end(),
                   [&](const auto &P) { return P.Property == Property; });
  if (Value == Properties.end())
    return Unsupported("queried property has no supplied observation");
  if (!Value->Bytes) {
    // The native registry-backed wrapper seeds ResultLength with the caller's
    // capacity; STATUS_NOT_FOUND bypasses the success/short-buffer update.
    if (auto E = Memory.writeInteger(A[4], Capacity, 4))
      return E;
    return uint64_t(0xc0000034); // STATUS_OBJECT_NAME_NOT_FOUND
  }
  const auto &Bytes = *Value->Bytes;
  const bool Fits = Capacity >= Bytes.size();
  if (Fits) {
    if (auto E = Writable(A[3], Bytes.size()))
      return E;
    // Output overlap involving ResultLength is outside this finite provider;
    // native registry and property-specific conversion have different stores.
    if (A[3] < A[4] + 4 && A[4] < A[3] + Bytes.size())
      return Unsupported("overlapping property output and ResultLength");
    if (auto E = Memory.write(A[3], Bytes))
      return E;
  }
  if (auto E = Memory.writeInteger(A[4], Bytes.size(), 4))
    return E;
  return uint64_t(Fits ? 0 : 0xc0000023);
}
} // namespace neverd::emulation
