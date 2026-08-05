//===- SignatureMatcher.h - FLIRT pattern matching engine ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Engine for matching FLIRT signature patterns against binary data.
///
/// Matching process:
///   1. Compare leading bytes (with wildcard mask) at candidate address.
///   2. Verify CRC16 of trailing bytes (after leading pattern).
///   3. Optionally verify tail bytes at known offsets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATUREMATCHER_H
#define NEVERD_SIGS_SIGNATUREMATCHER_H

#include "neverd/sigs/Signature.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace neverd {
namespace sigs {

class SignatureMatcher {
public:
  /// Compute the FLIRT CRC16 of a byte range.
  static uint16_t computeCRC16(const uint8_t *Data, size_t Len);

  /// Match a single pattern module against binary data at the given offset.
  /// \p Data points to the candidate function start.
  /// \p Available is the number of bytes available from that point.
  static bool matchPattern(const PatternModule &Mod, const uint8_t *Data,
                           size_t Available);

  /// Scan a region of binary data for all matching patterns from a module set.
  /// Calls \p Callback for each match with (offset, module_index).
  using MatchCallback =
      std::function<void(uint64_t Offset, const PatternModule &Mod)>;
  static void scanRegion(const uint8_t *Data, size_t Size,
                         const std::vector<PatternModule> &Modules,
                         MatchCallback Callback);

  /// Optimized scan: only check at known function entry points.
  /// Uses a hash index on leading non-wildcard bytes for O(1) bucket lookup.
  static void scanAtAddresses(const uint8_t *ImageBase, size_t ImageSize,
                              uint64_t BaseVA,
                              const std::vector<uint64_t> &FuncEntries,
                              const std::vector<PatternModule> &Modules,
                              MatchCallback Callback);

  /// Build a 2-byte hash index for fast dispatch.
  struct HashIndex {
    static constexpr size_t kBuckets = 65536;
    std::vector<std::vector<size_t>> Buckets;
    std::vector<size_t> WildcardBucket;
    void build(const std::vector<PatternModule> &Modules);
    uint16_t keyOf(const PatternModule &Mod) const;
    bool isWildcardKey(const PatternModule &Mod) const;
  };

private:
  static bool matchLeading(const std::vector<PatternByte> &Pattern,
                           const uint8_t *Data, size_t Available);

  static bool verifyCRC(const PatternModule &Mod, const uint8_t *Data,
                        size_t Available);

  static bool matchTail(const PatternModule &Mod, const uint8_t *Data,
                        size_t Available);
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATUREMATCHER_H
