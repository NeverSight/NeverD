//===- KernelGuardBitmap.h - Bounded native kernel CFG observation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded native kernel CFG observation.
///

#ifndef NEVERD_EMULATION_KERNELGUARDBITMAP_H
#define NEVERD_EMULATION_KERNELGUARDBITMAP_H
#include "neverd/emulation/GuestMemory.h"

#include <set>

namespace neverd::emulation {
class KernelReferenceImage;
/// A read-only projection of authenticated reference GFIDS. It does not replace
/// the session's stricter exact-entry guard or authorize reference execution.
class KernelGuardBitmap {
public:
  static constexpr uint64_t StoreBase = 0xfffff90010000000ULL;
  explicit KernelGuardBitmap(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error initialize(const KernelReferenceImage &Reference);
  llvm::Error resolveExport(uint64_t Address);
  llvm::Error validateAccess(uint64_t Address, uint32_t Size, bool Write) const;
  uint64_t pointer() const { return Bias; }

private:
  llvm::Error mark(uint64_t Address);
  GuestMemory &Memory;
  uint64_t Bias = 0, Base = 0, ImageSize = 0, DataSize = 0, MappingSize = 0;
  std::vector<uint8_t> Bytes;
  std::set<uint64_t> SuppressedExports;
};
} // namespace neverd::emulation
#endif
