//===- AndroidDlfcn.h - Explicit dynamic linker observations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared admission and storage policy for bounded positive dladdr queries.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_OS_LINUX_ANDROID_ANDROIDDLFCN_H
#define NEVERD_EMULATION_OS_LINUX_ANDROID_ANDROIDDLFCN_H

#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/Limits.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation::android_model {
namespace dladdr_model {
inline constexpr uint64_t MaximumRecords =
    ::neverd::limits::emulation::kAndroidDladdrObservationCount;
inline constexpr uint64_t MaximumStringBytes =
    ::neverd::limits::emulation::kAndroidDladdrStringBytes;
inline constexpr uint64_t MaximumSnapshotBytes =
    ::neverd::limits::emulation::kAndroidDladdrSnapshotBytes;
inline constexpr uint64_t InfoBytes = 4 * sizeof(uint64_t);
// Model-owned read-only strings, separate from JNI, calendar and error arenas.
inline constexpr uint64_t StorageBase = 0x7050000000;
} // namespace dladdr_model
llvm::Error validateDladdrObservations(
    const std::optional<std::vector<NativeDladdrObservation>> &Observations);
} // namespace neverd::emulation::android_model
#endif // NEVERD_EMULATION_OS_LINUX_ANDROID_ANDROIDDLFCN_H
