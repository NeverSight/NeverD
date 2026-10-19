//===- KernelSnapshotMemory.h - Stopped model RAM observations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stopped model RAM observations.
///

#ifndef NEVERD_EMULATION_KERNELSNAPSHOTMEMORY_H
#define NEVERD_EMULATION_KERNELSNAPSHOTMEMORY_H
#include "neverd/emulation/GuestMemory.h"

namespace neverd::emulation {
/// Explicit host observation of existing RAM. This never acknowledges a CPU
/// continuation, invokes MMIO, repairs a mapping or grants guest permissions.
/// Integer decoding remains in GuestMemory; mutation is always rejected.
class KernelSnapshotMemory final : public GuestMemory {
public:
  explicit KernelSnapshotMemory(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Memory.snapshotBacking(Address, Bytes);
  }
  llvm::Error map(uint64_t, uint64_t, unsigned) override { return immutable(); }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return immutable();
  }
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override {
    return immutable();
  }

private:
  static llvm::Error immutable() {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "kernel RAM snapshot is read-only");
  }
  GuestMemory &Memory;
};
} // namespace neverd::emulation
#endif
