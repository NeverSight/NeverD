//===- HostMemory.h - The host's physical memory ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The physical memory of the host, which bounds the caches that scale with
/// it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_HOSTMEMORY_H
#define NEVERD_SUPPORT_HOSTMEMORY_H

#include <cstdint>
#include <optional>

namespace neverd {

/// The bytes of physical memory the host has, where it says.
std::optional<uint64_t> hostPhysicalMemoryBytes();

} // namespace neverd

#endif // NEVERD_SUPPORT_HOSTMEMORY_H
