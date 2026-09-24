//===- ExecutableCodeOwnerIndex.h - Scoped code ownership -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXECUTABLECODEOWNERINDEX_H
#define NEVERD_LOADER_EXECUTABLECODEOWNERINDEX_H

#include "neverd/Common.h"

#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

struct BinaryImage;

/// Index the slow metadata lookups for one unchanged image operation. The
/// caller must rebuild after changing imports, symbols, ranges, mappings or
/// architecture/mode. Const queries may be shared by all operation workers;
/// no cached state is attached to the publicly mutable BinaryImage.
class ExecutableCodeOwnerIndex {
public:
  explicit ExecutableCodeOwnerIndex(const BinaryImage &Image);

  /// Conservative work bound for one exact fragment query, or no bound when
  /// this index belongs to another image. Construction is operation-scoped;
  /// callers must still charge all unrelated live metadata queries.
  std::optional<size_t> fragmentLookupWork(const BinaryImage &Image) const;

private:
  friend struct BinaryImage;
  friend bool
  isExplicitlyOwnedFunctionFragment(const BinaryImage &, va_t, va_t,
                                    const ExecutableCodeOwnerIndex *);

  bool isImportStubAt(va_t Addr) const;
  bool hasFunctionSymbolAt(va_t Addr) const;
  bool hasKnownOrTypedOwnerAt(va_t Addr) const;
  va_t getFunctionMetadataEnd(va_t Entry) const;
  bool ownsFunctionFragment(va_t Entry, va_t Target) const;

  const BinaryImage *Image;
  std::vector<va_t> ImportStubs;
  std::vector<std::pair<va_t, va_t>> ImportRanges;
  std::vector<va_t> FunctionStarts;
  std::vector<std::pair<va_t, va_t>> CodeRanges;
  // Exact raw entries and their smallest finite end, without normalization
  // or merging overlapping functions into one owner.
  std::vector<std::pair<va_t, va_t>> FunctionMetadataEnds;
  // (raw primary entry, fragment begin, fragment end). Ranges merge only
  // within one exact primary owner; callable entries remain a separate fact.
  std::vector<std::tuple<va_t, va_t, va_t>> FunctionFragments;
};

/// Exact runtime metadata must link a non-primary range to an existing primary
/// function. Indexed and live queries share the same relationship rules; a
/// null or foreign index uses the live image.
bool isExplicitlyOwnedFunctionFragment(
    const BinaryImage &Image, va_t FunctionEntry, va_t Target,
    const ExecutableCodeOwnerIndex *Index = nullptr);

/// Work envelope for that exact query, including the live fallback for an
/// absent or foreign index. No value means the envelope overflows size_t.
std::optional<size_t> explicitFunctionFragmentLookupWork(
    const BinaryImage &Image, const ExecutableCodeOwnerIndex *Index = nullptr);

} // namespace neverd

#endif // NEVERD_LOADER_EXECUTABLECODEOWNERINDEX_H
