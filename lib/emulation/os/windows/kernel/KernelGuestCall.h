//===- KernelGuestCall.h - Model-owned guest continuations ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Identify the model operation resumed after an actual guest callback. Token
/// identities are local to their owner; neither callback return values nor IRP
/// completion statuses are encoded in a token.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELGUESTCALL_H
#define NEVERD_EMULATION_KERNELGUESTCALL_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {

enum class GuestCallOwner {
  Framework,
  WDM,
  Interrupt,
  DMA,
  PoFx,
  UsbIdle,
  ExecutiveCallback,
  ProcessNotify,
  RegistryCallback,
  ThreadNotify,
  Avl,
  DriverService
};

struct GuestCallToken {
  GuestCallOwner Owner = GuestCallOwner::Framework;
  uint64_t ID = 0;
};

struct KernelGuestCall {
  GuestCallToken Token;
  uint64_t PC = 0;
  std::vector<uint64_t> Arguments;
  uint64_t SynchronizationObject = 0;
  /// Internal object callbacks can attach this same thread to the handle's
  /// process. This does not change its creating process or PreviousMode.
  std::optional<uint32_t> ProcessID;
};

} // namespace neverd::emulation

#endif
