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
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace llvm {
class SHA256;
} // namespace llvm

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

/// The SHA-256 digest of bytes given in parts: sha256() of them joined, from
/// the same compression, so a stream of formatted text is hashed with the
/// processor's SHA extensions where it has them.
class SHA256Stream {
public:
  SHA256Stream();
  SHA256Stream(const SHA256Stream &) = delete;
  SHA256Stream &operator=(const SHA256Stream &) = delete;
  ~SHA256Stream();

  void update(llvm::ArrayRef<uint8_t> Data);
  void update(llvm::StringRef Data) {
    update(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  }
  /// The digest of every byte given since the stream started or last gave
  /// one; the stream then starts again.
  std::array<uint8_t, 32> final();

private:
  void start();

  /// The compression state and the bytes of the block not yet compressed.
  std::array<uint32_t, 8> State = {};
  std::array<uint8_t, 64> Pending = {};
  size_t PendingBytes = 0;
  uint64_t Length = 0;
  /// The LLVM implementation, on a processor without the extensions.
  std::unique_ptr<llvm::SHA256> Portable;
};

} // namespace neverd

#endif // NEVERD_SUPPORT_SHA256_H
