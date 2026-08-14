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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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

  /// Bytes a module states exactly, across its leading pattern and its tail.
  ///
  /// A wildcard stands for a byte the linker rewrites, so a pattern made
  /// mostly of them agrees with far more code than it describes.  Counting
  /// what is left is how a caller sets a floor on that.
  static size_t fixedByteCount(const PatternModule &Mod);

  /// True when the leading pattern, the CRC span, and the tail together reach
  /// the end of the function the module claims to describe.
  ///
  /// A module that stops short still matches soundly, but it has only checked
  /// a prefix: two routines sharing a prologue and a first block are the same
  /// thing to it.  That is tolerable when the answer is a name shown to a
  /// reader and not when it is a name the rest of the analysis will act on --
  /// a personality decides which schema the language data is decoded with, so
  /// a caller settling one asks for whole-function agreement first.
  static bool isFullyVerified(const PatternModule &Mod);

  /// Scan a region of binary data for all matching patterns from a module set.
  /// Calls \p Callback with the region offset and matching module.
  using MatchCallback =
      std::function<void(uint64_t Offset, const PatternModule &Mod)>;

  /// Sparse byte-decision index for the fixed-width leading pattern.
  ///
  /// At a decision, matching can continue through only the edge for the input
  /// byte and the wildcard edge.  Modules are partitioned rather than copied,
  /// so each reaches exactly one leaf and is fully verified at most once.
  struct HashIndex {
    static constexpr size_t kIndexedBytes = 32;
    static constexpr size_t kLeafCandidates = 64;
    static constexpr size_t kNoNode = std::numeric_limits<size_t>::max();

    struct Edge {
      uint8_t Value = 0;
      size_t Child = kNoNode;
    };

    struct Node {
      uint8_t Offset = 0;
      size_t WildcardChild = kNoNode;
      std::vector<Edge> ExactChildren;
      std::vector<size_t> Candidates;

      bool isLeaf() const { return !Candidates.empty(); }
    };

    std::vector<Node> Nodes;
    size_t Root = kNoNode;

    void build(const std::vector<PatternModule> &Modules);
    uint16_t keyOf(const PatternModule &Mod) const;
    bool isWildcardKey(const PatternModule &Mod) const;

    /// Full pattern verifications scheduled for one input prefix.  This is a
    /// deterministic performance counter; it does not perform a match.
    size_t candidateCount(const uint8_t *Data, size_t Available) const;
  };

  static void scanRegion(const uint8_t *Data, size_t Size,
                         const std::vector<PatternModule> &Modules,
                         MatchCallback Callback);

  /// Optimized scan: only check at known function entry points.
  /// Uses the sparse leading-byte decision index above.
  static void scanAtAddresses(const uint8_t *ImageBase, size_t ImageSize,
                              uint64_t BaseVA,
                              const std::vector<uint64_t> &FuncEntries,
                              const std::vector<PatternModule> &Modules,
                              MatchCallback Callback);

  /// Scan with an index already built for \p Modules.  Reusing one across
  /// executable segments avoids rebuilding a large database for each segment.
  static void scanAtAddresses(const uint8_t *ImageBase, size_t ImageSize,
                              uint64_t BaseVA,
                              const std::vector<uint64_t> &FuncEntries,
                              const std::vector<PatternModule> &Modules,
                              const HashIndex &Index, MatchCallback Callback);

private:
  static bool matchLeading(const std::vector<PatternByte> &Pattern,
                           const uint8_t *Data, size_t Count);

  static bool verifyCRC(const PatternModule &Mod, const uint8_t *Data,
                        size_t MatchLimit);

  static bool matchTail(const PatternModule &Mod, const uint8_t *Data,
                        size_t MatchLimit);
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATUREMATCHER_H
