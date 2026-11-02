//===- SHA256.h - Shared one-shot SHA-256 ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The shared SHA-256 digest for immutable input bytes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_SHA256_H
#define NEVERD_SUPPORT_SHA256_H

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>

namespace neverd {

struct LoadProgress;

/// The SHA-256 digest of \p Data, the same as llvm::SHA256::hash's.
///
/// Uses x86 or little-endian ARM64 SHA extensions only when the required host
/// features are present, and the LLVM implementation on other processors.
std::array<uint8_t, 32> sha256(llvm::ArrayRef<uint8_t> Data);

/// Same digest and hardware implementation, with bounded byte checkpoints.
/// Reports "hash" progress and throws LoadCancelled on an observed stop.
std::array<uint8_t, 32> sha256(llvm::ArrayRef<uint8_t> Data,
                               const LoadProgress &Progress);

} // namespace neverd

#endif // NEVERD_SUPPORT_SHA256_H
