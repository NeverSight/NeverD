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
#include <vector>

namespace neverd::emulation {

enum class GuestCallOwner { Framework, WDM, Interrupt, DMA };

struct GuestCallToken {
  GuestCallOwner Owner = GuestCallOwner::Framework;
  uint64_t ID = 0;
};

struct KernelGuestCall {
  GuestCallToken Token;
  uint64_t PC = 0;
  std::vector<uint64_t> Arguments;
};

} // namespace neverd::emulation

#endif
