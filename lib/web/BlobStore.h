//===- BlobStore.h - Private immutable blob storage --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private immutable blob storage.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Blob.h"

#include <string_view>

namespace neverd::web {
/// Capture-only builder. It owns one private, unlinked spool per snapshot,
/// regardless of the number of members. No view is readable until sealing.
class BlobStore {
public:
  explicit BlobStore(uint64_t Budget);
  BlobStore(const BlobStore &) = delete;
  BlobStore &operator=(const BlobStore &) = delete;
  struct Captured {
    Blob Content;
    std::string Hash;
  };
  Captured capture(int Descriptor, uint64_t ExpectedSize);
  /// Append in-process derived bytes; no member name ever becomes a host path.
  /// Views remain unreadable until the complete store is sealed.
  void append(std::string_view Bytes);
  void seal();
  Blob whole() const;

private:
  void initialize();
  std::shared_ptr<Blob::Storage> State;
  uint64_t Budget;
  uint64_t Used = 0;
  bool Finished = false;
  bool Failed = false;
};
} // namespace neverd::web
