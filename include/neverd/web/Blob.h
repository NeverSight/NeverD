#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace neverd::web {
class BlobStore;

inline constexpr uint64_t MaxBlobReadBytes = 8 * 1024 * 1024;
inline constexpr uint64_t BlobTransferBytes = 64 * 1024;

/// An immutable byte range in an owned snapshot. Copies and slices share the
/// storage lifetime; they never reopen the selected input or expose a host
/// path. Materialization is explicit and bounded before allocation.
class Blob {
public:
  Blob() = default;
  uint64_t size() const { return Length; }
  Blob slice(uint64_t Offset, uint64_t Size) const;
  std::string read(uint64_t Offset, uint64_t Size,
                   uint64_t Budget = MaxBlobReadBytes) const;
  std::string digest() const;

private:
  struct Storage;
  std::shared_ptr<const Storage> Owner;
  uint64_t Start = 0;
  uint64_t Length = 0;
  Blob(std::shared_ptr<const Storage> Owner, uint64_t Start, uint64_t Length);
  friend class BlobStore;
};
} // namespace neverd::web
