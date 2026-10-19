//===- DriverKernelMemory.h - Bounded kernel RAM capacity -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded kernel RAM capacity.
///

#ifndef NEVERD_EMULATION_DRIVERKERNELMEMORY_H
#define NEVERD_EMULATION_DRIVERKERNELMEMORY_H

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::emulation {
constexpr uint64_t driverKernelMemorySize(std::optional<uint64_t> Size) {
  return Size.value_or(profile::KernelArenaSize);
}
inline llvm::Error validateDriverKernelMemorySize(std::optional<uint64_t> Size,
                                                  uint64_t MemoryLimit) {
  if (Size && (*Size < profile::KernelArenaSize ||
               *Size > profile::MaxKernelArenaSize ||
               *Size % profile::PageSize || *Size > MemoryLimit))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "kernel_memory_size must be page aligned, between 1 and 8 MiB, "
        "and no greater than memory_limit");
  return llvm::Error::success();
}
} // namespace neverd::emulation
#endif
