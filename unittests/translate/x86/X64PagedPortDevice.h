//===- X64PagedPortDevice.h - Independent synthetic port tape ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Feed KVM and the paged executor independent copies of the same device.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDPORTDEVICE_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDPORTDEVICE_H

#include "neverd/analysis/arch/x86_64/X64PagedPortIO.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace {
struct PagedPortTape {
  struct Entry {
    bool Input;
    uint16_t Port;
    unsigned Bytes;
    uint32_t Value;
    bool operator==(const Entry &) const = default;
  };
  std::vector<Entry> Transfers;
  bool Ready = true, Fail = false;
  std::optional<unsigned> StopAfter;
  uint32_t exchange(bool Input, uint16_t Port, unsigned Bytes, uint32_t Value) {
    const uint32_t Mask = Bytes == 4 ? UINT32_MAX : (1u << (Bytes * 8)) - 1;
    if (Input)
      Value = (0x87654321u + Transfers.size() * 0x123u) & Mask;
    Transfers.push_back({Input, Port, Bytes, Value});
    return Value;
  }
  static llvm::Expected<std::optional<uint32_t>>
  transfer(void *Context, const neverd::analysis::X64PortIORequest &Request) {
    auto &T = *static_cast<PagedPortTape *>(Context);
    if (T.Fail)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "synthetic device failure");
    if (!T.Ready || (T.StopAfter && T.Transfers.size() >= *T.StopAfter))
      return std::optional<uint32_t>{};
    return std::optional<uint32_t>{
        T.exchange(Request.Input, Request.Port, Request.Bytes, Request.Value)};
  }
  static void kvm(void *Context, bool Input, uint16_t Port, unsigned Bytes,
                  unsigned Count, uint8_t *Data) {
    auto &T = *static_cast<PagedPortTape *>(Context);
    for (unsigned I = 0; I != Count; ++I) {
      uint32_t Value = 0;
      if (!Input)
        for (unsigned J = 0; J != Bytes; ++J)
          Value |= uint32_t(Data[I * Bytes + J]) << (8 * J);
      Value = T.exchange(Input, Port, Bytes, Value);
      if (Input)
        for (unsigned J = 0; J != Bytes; ++J)
          Data[I * Bytes + J] = Value >> (8 * J);
    }
  }
};

} // namespace

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDPORTDEVICE_H
