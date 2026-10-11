//===- COFFRegistrationFrameTaint.h - Indexed PE32 frame taint ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a monotone union of pointer-bearing bytes for each private allocation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONFRAMETAINT_H
#define NEVERD_COFFREGISTRATIONFRAMETAINT_H

#include "llvm/ADT/DenseMap.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace llvm {
class Value;
}

namespace neverd::coff_registration {
class RegistrationFrameTaint {
public:
  /// Return whether the union grew or the query overlaps, respectively.
  /// An invalid range or exhausted shared budget has no successful result.
  std::optional<bool> insert(const llvm::Value *Root, int64_t Offset,
                             uint64_t Bytes, size_t &Work);
  std::optional<bool> overlaps(const llvm::Value *Root, int64_t Offset,
                               uint64_t Bytes, size_t &Work) const;

private:
  using Intervals = std::map<uint64_t, uint64_t>;
  llvm::DenseMap<const llvm::Value *, Intervals> Ranges;
};
} // namespace neverd::coff_registration

#endif
