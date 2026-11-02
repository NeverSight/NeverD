//===- ReadOnlyBytesScope.h - Immutable fixup indexes ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse vector-fixup lookup storage during an immutable image analysis.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_READONLYBYTESSCOPE_H
#define NEVERD_LOADER_READONLYBYTESSCOPE_H

#include <memory>

namespace neverd {
struct BinaryImage;
namespace read_only_detail {
struct VectorFixupIndex;
}

/// Optional PE32 storage-query acceleration on the calling thread. The image
/// and all its metadata must remain unchanged until this scope ends. Scopes
/// nest on the same thread; other images and oversized inventories use the
/// ordinary scans. Budgeted instruction indexes may share a complete sorted
/// inventory within this scope. Readers retain their validators and full
/// budget charges, including each instruction index's construction cost.
class ReadOnlyBytesScope {
public:
  explicit ReadOnlyBytesScope(const BinaryImage &Image);
  ~ReadOnlyBytesScope();
  ReadOnlyBytesScope(const ReadOnlyBytesScope &) = delete;
  ReadOnlyBytesScope &operator=(const ReadOnlyBytesScope &) = delete;

private:
  const read_only_detail::VectorFixupIndex *Previous;
  std::unique_ptr<read_only_detail::VectorFixupIndex> Index;
};
} // namespace neverd

#endif // NEVERD_LOADER_READONLYBYTESSCOPE_H
