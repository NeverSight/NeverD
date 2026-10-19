//===- KernelReferenceExports.h - Immutable PE export observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable PE export observations.
///

#ifndef NEVERD_EMULATION_KERNELREFERENCEEXPORTS_H
#define NEVERD_EMULATION_KERNELREFERENCEEXPORTS_H
#include "neverd/emulation/ImageMapping.h"

#include <map>
#include <string>
#include <utility>

namespace llvm::object {
class COFFObjectFile;
}
namespace neverd {
struct BinaryImage;
namespace emulation {
/// Validated RVA metadata only; never snapshots the runtime contents of data
/// exports. Page padding remains opaque even though the backend maps a page.
class KernelReferenceExports {
public:
  static llvm::Expected<KernelReferenceExports>
  load(const BinaryImage &Image, const llvm::object::COFFObjectFile &Object,
       uint64_t GuestBase, uint64_t MemoryLimit);
  const std::vector<ImageMapping> &pages() const { return Pages; }
  const std::map<std::string, uint32_t> &names() const { return Names; }
  bool permitsRead(uint64_t Address, uint32_t Size) const;
  bool overlapsBytes(uint64_t Address, uint64_t Size) const;
  bool overlapsPage(uint64_t Address, uint64_t Size) const;

private:
  std::vector<ImageMapping> Pages;
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  std::map<std::string, uint32_t> Names;
};
} // namespace emulation
} // namespace neverd
#endif
